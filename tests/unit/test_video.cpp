/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <src/video.h>
#include <src/encoder_probe_reuse.h>
#include <src/launch_failure.h>
#include <src/stream_stats.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <thread>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef __linux__
#include <src/platform/linux/encoder_auto_policy.h>
#include <src/platform/linux/encoder_probe_driver_proof.h>
#include <src/platform/linux/stream_display_policy.h>
#include <src/logging.h>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#endif

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#ifdef __linux__
    &video::vaapi,
#endif
#ifdef POLARIS_BUILD_VULKAN
    // Exercises the Vulkan Video device and a real probe encode in a binary that also links the
    // compute codec's volk, whose globals share the Vulkan entry points' names.
    &video::vulkan,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

#ifdef POLARIS_TESTS
namespace {
  struct LinuxDisplayConfigGuard {
    LinuxDisplayConfigGuard():
        auto_manage_displays {config::video.linux_display.auto_manage_displays},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        headless_mode {config::video.linux_display.headless_mode} {
    }

    ~LinuxDisplayConfigGuard() {
      config::video.linux_display.auto_manage_displays = auto_manage_displays;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.headless_mode = headless_mode;
    }

    bool auto_manage_displays;
    bool use_cage_compositor;
    bool headless_mode;
  };
}  // namespace

TEST(VideoColorSpaceTests, ClientHdrRequestWithoutDisplayMetadataStaysSdrMain10) {
  video::config_t config {};
  config.encoderCscMode = 2;  // Rec. 709 SDR
  config.dynamicRange = 1;

  const auto colorspace = video::colorspace_from_client_config(config, false);

  EXPECT_FALSE(video::colorspace_is_hdr(colorspace));
  EXPECT_EQ(colorspace.colorspace, video::colorspace_e::rec709);
  EXPECT_EQ(colorspace.bit_depth, 10);
}

TEST(VideoColorSpaceTests, ClientHdrRequestWithDisplayMetadataEnablesTrueHdr) {
  video::config_t config {};
  config.encoderCscMode = 2;  // ignored for true HDR
  config.dynamicRange = 1;

  const auto colorspace = video::colorspace_from_client_config(config, true);

  EXPECT_TRUE(video::colorspace_is_hdr(colorspace));
  EXPECT_EQ(colorspace.colorspace, video::colorspace_e::bt2020);
  EXPECT_EQ(colorspace.bit_depth, 10);
}

TEST(VideoCodecProfileTests, HevcProfileFollowsActualEncoderInputDepth) {
  EXPECT_EQ(video::hevc_profile_for_input_for_tests(8, 0), AV_PROFILE_HEVC_MAIN);
  EXPECT_EQ(video::hevc_profile_for_input_for_tests(10, 0), AV_PROFILE_HEVC_MAIN_10);
  EXPECT_EQ(video::hevc_profile_for_input_for_tests(8, 1), AV_PROFILE_HEVC_REXT);
  EXPECT_EQ(video::hevc_profile_for_input_for_tests(10, 1), AV_PROFILE_HEVC_REXT);
}

namespace {
  SS_HDR_METADATA usable_hdr_metadata() {
    SS_HDR_METADATA metadata {};
    metadata.displayPrimaries[0].x = 34000;
    metadata.displayPrimaries[0].y = 16000;
    metadata.displayPrimaries[1].x = 13250;
    metadata.displayPrimaries[1].y = 34500;
    metadata.displayPrimaries[2].x = 7500;
    metadata.displayPrimaries[2].y = 3000;
    metadata.whitePoint.x = 15635;
    metadata.whitePoint.y = 16450;
    metadata.maxDisplayLuminance = 1000;
    metadata.minDisplayLuminance = 1;
    return metadata;
  }
}  // namespace

TEST(VideoHdrMetadataTests, AcceptsMetadataWithPrimariesAndMasteringLuminance) {
  auto metadata = usable_hdr_metadata();

  EXPECT_TRUE(video::hdr_metadata_is_usable_for_tests(metadata));
}

TEST(VideoHdrMetadataTests, RejectsMetadataWithoutMasteringLuminance) {
  auto metadata = usable_hdr_metadata();
  metadata.maxDisplayLuminance = 0;

  EXPECT_FALSE(video::hdr_metadata_is_usable_for_tests(metadata));
}

TEST(VideoHdrMetadataTests, RejectsMetadataWithoutDisplayPrimaries) {
  auto metadata = usable_hdr_metadata();
  metadata.displayPrimaries[1].x = 0;

  EXPECT_FALSE(video::hdr_metadata_is_usable_for_tests(metadata));
}

TEST(VideoCacheTests, DriverVersionCacheHitRequiresMatchingBinaryMetadata) {
  const auto cache_dir = std::filesystem::temp_directory_path() / "polaris-video-cache-tests";
  const auto cache_path = cache_dir / "driver_version_cache.txt";
  const auto binary_path = cache_dir / "nvidia-smi";

  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);
  ASSERT_FALSE(ec);

  ASSERT_TRUE(video::write_driver_version_cache_for_tests(cache_path, binary_path, "12345", "570.144"));
  EXPECT_EQ(video::read_driver_version_cache_for_tests(cache_path, binary_path, "12345"), "570.144");
  EXPECT_TRUE(video::read_driver_version_cache_for_tests(cache_path, binary_path, "54321").empty());
  EXPECT_TRUE(video::read_driver_version_cache_for_tests(cache_path, cache_dir / "other-nvidia-smi", "12345").empty());

  std::filesystem::remove_all(cache_dir, ec);
}

TEST(VideoCacheTests, NvencFallbackNamesTheDriverAndWhereToLook) {
  // An nvenc encoder that cannot open because the linked FFmpeg wants a newer
  // API than the driver provides used to surface as nothing at all: the stream
  // silently dropped to software. Polaris 1.4.6 stopped discarding libav's own
  // error, which names the required and found versions; this points at it.
  const auto detail = video::nvenc_fallback_detail("nvenc", "software", "580.178.04");
  EXPECT_NE(detail.find("580.178.04"), std::string::npos)
    << "the reason must name the driver actually running";
  EXPECT_NE(detail.find("nvenc API version"), std::string::npos)
    << "and must send the reader to the line that names what was required";

  // Silent everywhere it would be guessing.
  EXPECT_TRUE(video::nvenc_fallback_detail("nvenc", "nvenc", "580.178.04").empty())
    << "nvenc started, so there is nothing to explain";
  EXPECT_TRUE(video::nvenc_fallback_detail("vaapi", "software", "580.178.04").empty())
    << "a vaapi fallback is not an nvenc driver problem";
  EXPECT_TRUE(video::nvenc_fallback_detail("nvenc", "software", "").empty())
    << "without a known driver version there is no fact to report";
}

#if defined(POLARIS_TESTS) && defined(__linux__)
TEST(VideoEncoderSelectionTests, AnAutoFallbackOpensItsReasonWithTheEncoderItFellBackTo) {
  // A fallback is the one case the Doctor grades encoder selection watch, and so the one case Nova's
  // Android Doctor card can show the reason, in two lines after its own text. On AMD outside labwc
  // the reason opened "Auto uses VA-API" and put the fallback last, about 560 characters on, so a
  // host that fell back to software read that it was on VA-API.
  video::encoder_selection_info_t amd;
  amd.mode = "auto";
  amd.gpu_driver = "amdgpu";
  amd.policy = "amd_established_desktop";
  amd.preferred_encoder = "vaapi";
  amd.fallback_encoder = "next_available";
  amd.reason = std::string {linux_encoder_auto_policy::reason(amd.policy, true, true)};
  const auto policy_sentence = amd.reason;

  auto fell_back = amd;
  video::finalize_encoder_selection_info_for_tests(fell_back, "software");
  ASSERT_TRUE(fell_back.fallback_used);
  EXPECT_EQ(
    fell_back.reason,
    "Preferred encoder [vaapi] did not satisfy this runtime; selected [software] instead. " + policy_sentence
  );
  EXPECT_EQ(fell_back.reason.find("uses VA-API"), std::string::npos) << fell_back.reason;

  // Where Auto got the encoder it prefers, the policy sentence still opens the reason.
  auto landed = amd;
  video::finalize_encoder_selection_info_for_tests(landed, "vaapi");
  EXPECT_FALSE(landed.fallback_used);
  EXPECT_EQ(landed.reason, policy_sentence + " Selected [vaapi].");

  // The NVENC driver detail explains the fallback, so it stays beside it, ahead of the policy.
  video::encoder_selection_info_t nvidia;
  nvidia.mode = "auto";
  nvidia.gpu_driver = "nvidia";
  nvidia.policy = "nvidia_nvenc";
  nvidia.preferred_encoder = "nvenc";
  nvidia.fallback_encoder = "next_available";
  nvidia.driver_version = "580.178.04";
  nvidia.reason = std::string {linux_encoder_auto_policy::reason(nvidia.policy, true, true)};
  video::finalize_encoder_selection_info_for_tests(nvidia, "software");
  EXPECT_EQ(
    nvidia.reason,
    "Preferred encoder [nvenc] did not satisfy this runtime; selected [software] instead." +
      video::nvenc_fallback_detail("nvenc", "software", "580.178.04") + " Auto detected NVIDIA; prefer NVENC."
  );
}
#endif

TEST(VideoYuv444Tests, EncodersInThisBuildSayWhichCodecsTheirTablesCarryIn444) {
  // The console words its 4:4:4 row from this list, so it has to match the tables the probe reads.
  const auto listed = video::yuv444_encoders();
  ASSERT_FALSE(listed.empty());
  std::vector<std::string> names;
  for (const auto &[name, codecs] : listed) {
    names.emplace_back(name);
    EXPECT_NE(name, "pyrowave") << "the probe never chooses PyroWave, so it is not one of these";
    if (name == "software") {
      // libx264 in 4:4:4; H264_ONLY keeps HEVC out under Auto.
      EXPECT_EQ(codecs, (std::array<bool, 3> {true, false, false}));
    }
#ifdef __linux__
    else {
      // On Linux no GPU encoder table carries 4:4:4: NVENC, VA-API and Vulkan Video stream 4:2:0.
      EXPECT_EQ(codecs, (std::array<bool, 3> {false, false, false})) << name;
    }
#endif
  }
  EXPECT_EQ(names.back(), "software") << "the encoder of last resort comes last";
#ifdef __linux__
  EXPECT_NE(std::find(names.begin(), names.end(), "nvenc"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "vaapi"), names.end());
  #ifdef POLARIS_BUILD_VULKAN
  EXPECT_NE(std::find(names.begin(), names.end(), "vulkan"), names.end());
  #endif
#endif
}

TEST(VideoCacheTests, DriverVersionRejectsAnythingThatIsNotAVersion) {
  // nvidia-smi prints its NVML failure to stdout, so without this the banner
  // becomes the driver string, and because the cache is keyed on the tool's
  // path and mtime rather than its output it then survives every restart.
  EXPECT_EQ(video::parse_nvidia_driver_version_for_tests("610.57.04"), "610.57.04");
  EXPECT_EQ(video::parse_nvidia_driver_version_for_tests("570.144\n"), "570.144");
  EXPECT_EQ(video::parse_nvidia_driver_version_for_tests("610.57.04.01"), "610.57.04.01");

  for (const auto *banner : {
         "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver.",
         "Failed to initialize NVML: Driver/library version mismatch",
         "No devices were found",
         "",
         "610",
         "610.",
         ".57",
         "610..57",
         "610.57.04.01.02",
       }) {
    EXPECT_TRUE(video::parse_nvidia_driver_version_for_tests(banner).empty())
      << "accepted [" << banner << "] as a driver version";
  }
}

TEST(VideoCacheTests, DriverVersionCacheHealsItselfWhenAlreadyPoisoned) {
  // A cache written by an older build outlives the fix otherwise, because the
  // entry is only reconsidered when nvidia-smi itself changes on disk.
  const auto cache_dir = std::filesystem::temp_directory_path() / "polaris-video-cache-poison-tests";
  const auto cache_path = cache_dir / "driver_version_cache.txt";
  const auto binary_path = cache_dir / "nvidia-smi";

  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);
  ASSERT_FALSE(ec);

  ASSERT_TRUE(video::write_driver_version_cache_for_tests(
    cache_path,
    binary_path,
    "12345",
    "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver."
  ));
  EXPECT_TRUE(video::read_driver_version_cache_for_tests(cache_path, binary_path, "12345").empty())
    << "a poisoned driver cache entry must not be believed";

  std::filesystem::remove_all(cache_dir, ec);
}

TEST(VideoCacheTests, ResetDisplayRetryDelayBackoffCapsAtTwoHundredMilliseconds) {
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(0), std::chrono::milliseconds(50));
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(1), std::chrono::milliseconds(100));
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(2), std::chrono::milliseconds(200));
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(3), std::chrono::milliseconds(200));
}

TEST(VideoDisplaySelectionTests, ExactNamedCaptureDoesNotAllowGenericFallback) {
  EXPECT_TRUE(video::capture_fallback_allowed_for_tests(""));
  EXPECT_FALSE(video::capture_fallback_allowed_for_tests("POLARIS-HEADLESS-512536-0"));
}

TEST(VideoDisplaySelectionTests, ExactDisplayIdentityMustRemainPresentAcrossReinit) {
  const std::vector<std::string> displays {
    "POLARIS-HEADLESS-512536-0",
    "HDMI-A-1",
  };

  EXPECT_EQ(video::find_display_index_for_tests(displays, "POLARIS-HEADLESS-512536-0"), 0);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "HDMI-A-1"), 1);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "1"), 1)
    << "legacy numeric WLR selection must retain its index after connector-name enumeration";
  EXPECT_EQ(video::find_display_index_for_tests(displays, "2"), std::nullopt);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "missing-output"), std::nullopt);
}

TEST(VideoDisplaySelectionTests, ExactOwnedCaptureRejectsDisplaySwitches) {
  EXPECT_TRUE(video::display_switch_allowed_for_exact_capture_for_tests(""));
  EXPECT_FALSE(video::display_switch_allowed_for_exact_capture_for_tests("POLARIS-HEADLESS-512536-0"));
  EXPECT_FALSE(video::display_switch_allowed_for_exact_capture_for_tests("HDMI-A-1"));
}

TEST(VideoDisplaySelectionTests, CaptureContextsMustShareTheWholeGeneration) {
  capture_generation::identity_t generation {
    .generation_id = 42,
    .exact_display_name = "POLARIS-HEADLESS-512536-0",
    .requested_output_name = "POLARIS-HEADLESS-512536-0",
    .stream_mode = "host_virtual_display",
    .capture_backend = "portal",
    .private_wayland_socket = "wayland-polaris-42",
    .private_runtime_instance_id = "session-42",
    .adapter_name = "/dev/dri/renderD128",
    .headless_mode = true,
  };
  EXPECT_TRUE(video::capture_generations_match_for_tests(generation, generation));

  auto changed = generation;
  changed.generation_id = 43;
  EXPECT_FALSE(video::capture_generations_match_for_tests(generation, changed));
  changed = generation;
  changed.stream_mode = "desktop_display";
  EXPECT_FALSE(video::capture_generations_match_for_tests(generation, changed));
  changed = generation;
  changed.private_wayland_socket = "wayland-polaris-43";
  EXPECT_FALSE(video::capture_generations_match_for_tests(generation, changed));
  changed = generation;
  changed.private_runtime_instance_id = "session-43";
  EXPECT_FALSE(video::capture_generations_match_for_tests(generation, changed));
  changed = generation;
  changed.adapter_name = "/dev/dri/renderD129";
  EXPECT_FALSE(video::capture_generations_match_for_tests(generation, changed));
  changed = generation;
  changed.exact_display_name.clear();
  EXPECT_FALSE(video::capture_generations_match_for_tests(generation, changed));
}

TEST(VideoDisplaySelectionTests, RejectsDisplaySwitchWhenDisplayListIsEmpty) {
  EXPECT_EQ(video::clamp_display_index_for_tests(1, 0), std::nullopt);
}

TEST(VideoDisplaySelectionTests, ClampsDisplaySwitchToAvailableDisplayRange) {
  EXPECT_EQ(video::clamp_display_index_for_tests(-1, 3), 0);
  EXPECT_EQ(video::clamp_display_index_for_tests(1, 3), 1);
  EXPECT_EQ(video::clamp_display_index_for_tests(8, 3), 2);
}

TEST(VideoFrameConversionTests, InfersPackedBgr0InputStrideWhenRowPitchIsMissing) {
  EXPECT_EQ(video::software_frame_input_linesize_for_tests(0, 0, 0, 2560, AV_PIX_FMT_BGR0), 10240);
}

TEST(VideoFrameConversionTests, PrefersCaptureProvidedRowPitch) {
  EXPECT_EQ(video::software_frame_input_linesize_for_tests(8192, 4, 1920, 1920, AV_PIX_FMT_BGR0), 8192);
}

#ifdef __linux__
TEST(VideoNvencSplitEncodeModeTests, AppliesOnlyToLinuxFfmpegNvencHevcAndAv1) {
  EXPECT_TRUE(video::should_apply_nvenc_split_encode_mode_for_tests("nvenc", "hevc_nvenc"));
  EXPECT_TRUE(video::should_apply_nvenc_split_encode_mode_for_tests("nvenc", "av1_nvenc"));
  EXPECT_FALSE(video::should_apply_nvenc_split_encode_mode_for_tests("nvenc", "h264_nvenc"));
  EXPECT_FALSE(video::should_apply_nvenc_split_encode_mode_for_tests("vaapi", "hevc_vaapi"));
}
#endif

TEST(VideoNvencSplitEncodeModeTests, MapsConfigModesToFfmpegValues) {
  EXPECT_EQ(video::nvenc_split_encode_mode_value_for_tests(nvenc::nvenc_split_encode_mode::disabled), 15);
  EXPECT_EQ(video::nvenc_split_encode_mode_value_for_tests(nvenc::nvenc_split_encode_mode::auto_mode), 0);
  EXPECT_EQ(video::nvenc_split_encode_mode_value_for_tests(nvenc::nvenc_split_encode_mode::forced), 1);
  EXPECT_EQ(video::nvenc_split_encode_mode_value_for_tests(nvenc::nvenc_split_encode_mode::two_way), 2);
  EXPECT_EQ(video::nvenc_split_encode_mode_value_for_tests(nvenc::nvenc_split_encode_mode::three_way), 3);
}

#ifdef __linux__
TEST(VideoNvencSplitEncodeModeTests, DecidesToApplySupportedRequestedModeWhenFfmpegOptionExists) {
  const auto decision = video::nvenc_split_encode_mode_decision_for_tests(
    "nvenc",
    "hevc_nvenc",
    nvenc::nvenc_split_encode_mode::two_way,
    true
  );

  EXPECT_EQ(decision.decision, video::nvenc_split_encode_mode_decision_e::apply);
  EXPECT_EQ(decision.ffmpeg_value, 2);
  EXPECT_FALSE(decision.should_warn);
}

TEST(VideoNvencSplitEncodeModeTests, DecidesDisabledModeAppliesExplicitFfmpegDisableWhenOptionExists) {
  const auto decision = video::nvenc_split_encode_mode_decision_for_tests(
    "nvenc",
    "hevc_nvenc",
    nvenc::nvenc_split_encode_mode::disabled,
    true
  );

  EXPECT_EQ(decision.decision, video::nvenc_split_encode_mode_decision_e::apply);
  EXPECT_EQ(decision.ffmpeg_value, 15);
  EXPECT_FALSE(decision.should_warn);
}

TEST(VideoNvencSplitEncodeModeTests, DecidesDisabledModeSkipsWithoutWarningWhenFfmpegOptionIsMissing) {
  const auto decision = video::nvenc_split_encode_mode_decision_for_tests(
    "nvenc",
    "hevc_nvenc",
    nvenc::nvenc_split_encode_mode::disabled,
    false
  );

  EXPECT_EQ(decision.decision, video::nvenc_split_encode_mode_decision_e::disabled);
  EXPECT_EQ(decision.ffmpeg_value, std::nullopt);
  EXPECT_FALSE(decision.should_warn);
}

TEST(VideoNvencSplitEncodeModeTests, DecidesIneligibleCodecSkipsWithoutWarning) {
  const auto decision = video::nvenc_split_encode_mode_decision_for_tests(
    "nvenc",
    "h264_nvenc",
    nvenc::nvenc_split_encode_mode::two_way,
    true
  );

  EXPECT_EQ(decision.decision, video::nvenc_split_encode_mode_decision_e::unsupported_encoder_or_codec);
  EXPECT_EQ(decision.ffmpeg_value, std::nullopt);
  EXPECT_FALSE(decision.should_warn);
}

TEST(VideoNvencSplitEncodeModeTests, DecidesMissingFfmpegOptionWarnsForRequestedMode) {
  const auto decision = video::nvenc_split_encode_mode_decision_for_tests(
    "nvenc",
    "av1_nvenc",
    nvenc::nvenc_split_encode_mode::three_way,
    false
  );

  EXPECT_EQ(decision.decision, video::nvenc_split_encode_mode_decision_e::missing_ffmpeg_option);
  EXPECT_EQ(decision.ffmpeg_value, std::nullopt);
  EXPECT_TRUE(decision.should_warn);
}

TEST(VideoNvencSplitEncodeModeTests, SelectsFfmpegOptionOnlyForSupportedNvencCodecs) {
  EXPECT_EQ(
    video::nvenc_split_encode_mode_option_value_for_tests(
      "nvenc",
      "hevc_nvenc",
      nvenc::nvenc_split_encode_mode::two_way
    ),
    2
  );
  EXPECT_EQ(
    video::nvenc_split_encode_mode_option_value_for_tests(
      "nvenc",
      "av1_nvenc",
      nvenc::nvenc_split_encode_mode::three_way
    ),
    3
  );
  EXPECT_EQ(
    video::nvenc_split_encode_mode_option_value_for_tests(
      "nvenc",
      "h264_nvenc",
      nvenc::nvenc_split_encode_mode::two_way
    ),
    std::nullopt
  );
  EXPECT_EQ(
    video::nvenc_split_encode_mode_option_value_for_tests(
      "vaapi",
      "hevc_vaapi",
      nvenc::nvenc_split_encode_mode::two_way
    ),
    std::nullopt
  );
}
#endif

TEST(VideoCacheTests, EncoderProbeCachePersistsCodecModesAndInvalidatesOnTopologyMismatch) {
  const auto cache_dir = std::filesystem::temp_directory_path() / "polaris-encoder-cache-tests";
  const auto cache_path = cache_dir / "encoder_cache.txt";

  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);
  ASSERT_FALSE(ec);

  const video::codec_capability_state_t capability_state {
    3,
    3,
    {true, false, true}
  };

  ASSERT_TRUE(video::write_encoder_probe_cache_for_tests(
    cache_path,
    "580.142",
    "capture=;encoder=nvenc;adapter=;output=;cage=1;headless=1;auto_manage=0;displays=deferred-cage",
    "nvenc",
    capability_state
  ));

  const auto cached = video::read_encoder_probe_cache_for_tests(
    cache_path,
    "580.142",
    "capture=;encoder=nvenc;adapter=;output=;cage=1;headless=1;auto_manage=0;displays=deferred-cage"
  );
  EXPECT_EQ(cached.encoder_name, "nvenc");
  EXPECT_TRUE(cached.has_capability_data);
  EXPECT_EQ(cached.capability_state.hevc_mode, 3);
  EXPECT_EQ(cached.capability_state.av1_mode, 3);
  EXPECT_EQ(cached.capability_state.yuv444_for_codec, (std::array<bool, 3> {true, false, true}));

  const auto invalidated = video::read_encoder_probe_cache_for_tests(
    cache_path,
    "580.142",
    "capture=;encoder=nvenc;adapter=;output=;cage=1;headless=1;auto_manage=0;displays=1"
  );
  EXPECT_TRUE(invalidated.encoder_name.empty());
  EXPECT_FALSE(std::filesystem::exists(cache_path));

  std::filesystem::remove_all(cache_dir, ec);
}

TEST(VideoCacheTests, HeadlessCageUsesDeferredCageTopologyKeyForColdLaunchAdvertising) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.auto_manage_displays = false;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;
  const auto topology = video::current_encoder_topology_key_for_tests();

  EXPECT_NE(topology.find(";cage=1"), std::string::npos);
  EXPECT_NE(topology.find(";headless=1"), std::string::npos);
  EXPECT_NE(topology.find(";auto_manage=0"), std::string::npos);
  EXPECT_NE(topology.find(";displays=deferred-cage"), std::string::npos);
}
#endif

TEST(CaptureEventLifecycle, AtomicDrainCompetesWithConsumerWithoutWaiting) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    safe::event_t<std::shared_ptr<int>> event;
    auto value = std::make_shared<int>(iteration);
    std::weak_ptr<int> lifetime = value;
    event.raise(std::move(value));
    std::atomic<int> consumed {0};
    std::atomic<bool> start {false};
    const auto consume = [&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      if (event.try_pop()) ++consumed;
    };
    std::thread encoder {consume};
    std::thread drain {consume};
    start.store(true, std::memory_order_release);
    encoder.join();
    drain.join();
    EXPECT_EQ(consumed.load(), 1);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(event.try_pop());
  }
}

TEST(CaptureEventLifecycle, StopWakesCaptureConsumerAndRejectsFurtherFrames) {
  safe::event_t<std::shared_ptr<int>> event;
  std::thread consumer {[&] { EXPECT_FALSE(event.pop()); }};
  event.stop();
  consumer.join();
  event.raise(std::make_shared<int>(1));
  EXPECT_FALSE(event.peek());
  EXPECT_FALSE(event.running());
  EXPECT_FALSE(event.try_pop());
}

TEST(EncodedPacketLifecycle, ReplacementBytesSurviveTheirSourceAndSession) {
  auto packet = std::make_unique<video::packet_raw_avcodec>();
  {
    std::string original = "old parameter set";
    std::string replacement = "new parameter set";
    auto replacements = std::make_shared<std::vector<video::packet_raw_t::replace_t>>();
    replacements->emplace_back(original, replacement);
    packet->replacements = replacements;
    original.assign(4096, 'x');
    replacement.assign(4096, 'y');
  }
  ASSERT_EQ(packet->replacements->size(), 1);
  EXPECT_EQ(packet->replacements->front().old, "old parameter set");
  EXPECT_EQ(packet->replacements->front()._new, "new parameter set");
}

namespace {
  class LifetimeDisplay final: public platf::display_t {
  public:
    explicit LifetimeDisplay(std::vector<std::string> &order): order {order} {}
    ~LifetimeDisplay() override { order.emplace_back("display"); }
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override { return platf::capture_e::ok; }
    std::shared_ptr<platf::img_t> alloc_img() override { return {}; }
    int dummy_img(platf::img_t *) override { return -1; }
    std::vector<std::string> &order;
  };
  class LifetimeConverter final: public video::frame_converter_t {
  public:
    explicit LifetimeConverter(std::vector<std::string> &order): order {order} {}
    ~LifetimeConverter() override { order.emplace_back("converter"); }
    std::string_view name() const override { return "lifetime-test"; }
    bool supports(const video::frame_t &, const video::conversion_request_t &) const override { return false; }
    int convert(video::frame_t &, const video::conversion_request_t &) override { return -1; }
    std::vector<std::string> &order;
  };
}

TEST(EncodedPacketLifecycle, DriverReferencesReleaseOnEncoderThreadBeforePacketDelivery) {
  const auto owner = std::this_thread::get_id();
  std::vector<std::string> order;
  auto packet = std::make_unique<video::packet_raw_avcodec>();
  struct DriverBuffer {
    std::thread::id owner;
    std::vector<std::string> *order;
    std::unique_ptr<LifetimeConverter> converter;
  };
  auto driver = new DriverBuffer {owner, &order, std::make_unique<LifetimeConverter>(order)};
  packet->av_packet->buf = av_buffer_create(
    static_cast<uint8_t *>(av_malloc(AV_INPUT_BUFFER_PADDING_SIZE + 4)), 4,
    [](void *opaque, uint8_t *data) {
      auto driver = static_cast<DriverBuffer *>(opaque);
      EXPECT_EQ(std::this_thread::get_id(), driver->owner);
      driver->order->emplace_back("driver-buffer");
      delete driver;
      av_free(data);
    }, driver, 0);
  ASSERT_NE(packet->av_packet->buf, nullptr);
  packet->av_packet->data = packet->av_packet->buf->data;
  packet->av_packet->size = 4;
  std::memcpy(packet->av_packet->data, "data", 4);
  packet->av_packet->pts = 42;
  packet->av_packet->flags = AV_PKT_FLAG_KEY;
  packet->av_packet->opaque_ref = av_buffer_ref(packet->av_packet->buf);
  auto metadata = av_packet_new_side_data(packet->av_packet, AV_PKT_DATA_STRINGS_METADATA, 4);
  ASSERT_NE(metadata, nullptr);
  std::memcpy(metadata, "meta", 4);
  ASSERT_TRUE(packet->detach_encoder_buffer());
  EXPECT_EQ(order, (std::vector<std::string> {"driver-buffer", "converter"}));
  std::thread consumer {[packet = std::move(packet)]() mutable {
    EXPECT_EQ(packet->frame_index(), 42);
    EXPECT_TRUE(packet->is_idr());
    size_t metadata_size = 0;
    auto metadata = av_packet_get_side_data(packet->av_packet, AV_PKT_DATA_STRINGS_METADATA, &metadata_size);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<char *>(metadata), metadata_size), "meta");
    EXPECT_EQ(std::string_view(reinterpret_cast<char *>(packet->data()), packet->data_size()), "data");
    packet.reset();
  }};
  consumer.join();
  EXPECT_EQ(order.size(), 2);
}

TEST(CaptureEventLifecycle, CaptureReinitCanStopWhileConsumerRetainsDisplay) {
  std::vector<std::string> order;
  std::shared_ptr<platf::display_t> display = std::make_shared<LifetimeDisplay>(order);
  safe::queue_t<std::shared_ptr<platf::display_t>> packets;
  packets.raise(display);
  packets.stop();
  EXPECT_FALSE(packets.pop());
  safe::queue_t<int> capture_control;
  safe::signal_t draining;
  bool released = true;
  std::thread capture {[&] {
    released = video::wait_for_capture_display_release(display,
      [&] { return capture_control.running(); }, [&] { draining.raise(true); });
  }};
  const auto entered_wait = draining.pop(std::chrono::seconds(1));
  capture_control.stop();
  capture.join();
  EXPECT_TRUE(entered_wait);
  EXPECT_FALSE(released);
  EXPECT_TRUE(order.empty());
  EXPECT_EQ(display.use_count(), 2);
}

#ifdef POLARIS_TESTS
namespace {
  video::probe_reuse::identity_t complete_probe_identity() {
    return {"pci-gpu-device", "kernel-and-userspace-driver", "live-capture-generation", "capability-settings"};
  }
}

TEST(VideoProbeReuseTests, OnlyCompleteUnchangedSuccessfulIdentityCanReuse) {
  video::probe_reuse::cache_t cache;
  const auto identity = complete_probe_identity();
  EXPECT_FALSE(cache.reusable(identity, "nvenc", "", false));
  cache.remember(identity, identity, "nvenc", cache.epoch());
  EXPECT_TRUE(cache.reusable(identity, "nvenc", "", false));
  EXPECT_TRUE(cache.reusable(identity, "nvenc", "nvenc", false));
  EXPECT_FALSE(cache.reusable(identity, "vaapi", "", false));
  EXPECT_FALSE(cache.reusable(identity, "nvenc", "vaapi", false));
  EXPECT_FALSE(cache.reusable(identity, "nvenc", "", true));
  EXPECT_FALSE(cache.reusable(std::nullopt, "nvenc", "", false));
  for (auto member : {&video::probe_reuse::identity_t::gpu, &video::probe_reuse::identity_t::driver,
                      &video::probe_reuse::identity_t::topology, &video::probe_reuse::identity_t::settings}) {
    auto changed = identity;
    changed.*member += "-changed";
    EXPECT_FALSE(cache.reusable(changed, "nvenc", "", false));
    changed.*member = "";
    EXPECT_FALSE(cache.reusable(changed, "nvenc", "", false));
    cache.remember(changed, changed, "nvenc", cache.epoch());
    EXPECT_FALSE(cache.reusable(changed, "nvenc", "", false));
    cache.remember(identity, identity, "nvenc", cache.epoch());
  }
}

TEST(VideoProbeReuseTests, VulkanAndLastResortEncodersProbeLiveWhateverThePolicySays) {
  // #635: explicit encoder = vulkan logs exact_live_probe_required=0, because only Auto sets that
  // flag, yet a Vulkan probe is never reused. The reuse gate and the encoder_auto line both ask
  // this one rule, so the line cannot report reuse the gate would refuse.
  using video::probe_reuse::live_probe_mandatory;
  EXPECT_FALSE(live_probe_mandatory(false, false, "", "vaapi"));
  EXPECT_FALSE(live_probe_mandatory(false, false, "nvenc", "nvenc"));
  EXPECT_TRUE(live_probe_mandatory(false, true, "", "vaapi"));
  EXPECT_TRUE(live_probe_mandatory(false, false, "vulkan", "vulkan"));
  EXPECT_TRUE(live_probe_mandatory(false, false, "vulkan", "vaapi"));
  EXPECT_TRUE(live_probe_mandatory(false, false, "", "vulkan"));
  EXPECT_TRUE(live_probe_mandatory(true, false, "", "software"));
}

TEST(VideoProbeReuseTests, NextProbeLineSaysReuseOnlyWhereTheGateGivesIt) {
  // #635: next_probe_reuse read allowed wherever live_probe_mandatory said no, the reporter's Auto
  // VA-API route on Gamescope Stream included, where no probe identity exists and the gate never
  // reuses. The line now takes the gate's own answer and otherwise says why a fresh probe runs.
  using video::probe_reuse::next_probe_reuse;
  using video::probe_reuse::route_carries_identity;
  constexpr auto npos = std::string_view::npos;

  // Only NVENC on the labwc private compositor, capturing through wlr, carries an identity.
  EXPECT_TRUE(route_carries_identity(true, "wlr", "nvenc"));
  EXPECT_TRUE(route_carries_identity(true, "", "nvenc"));
  EXPECT_FALSE(route_carries_identity(true, "wlr", "vaapi"));
  EXPECT_FALSE(route_carries_identity(true, "wlr", "vulkan"));
  EXPECT_FALSE(route_carries_identity(true, "kms", "nvenc"));
  EXPECT_FALSE(route_carries_identity(false, "portal", "vaapi"));
  EXPECT_FALSE(route_carries_identity(false, "", "nvenc"));

  // Reuse is said only when the gate gives it, and never otherwise, whatever the other inputs.
  EXPECT_EQ(next_probe_reuse(true, false, false, "", "nvenc", true).rfind("if_unchanged (", 0), 0u);
  const std::string_view refused[] {
    next_probe_reuse(false, false, false, "vulkan", "vulkan", false),
    next_probe_reuse(false, false, false, "", "vulkan", true),
    next_probe_reuse(false, true, false, "", "software", false),
    next_probe_reuse(false, false, true, "", "vaapi", false),
    next_probe_reuse(false, false, false, "", "vaapi", false),
    next_probe_reuse(false, false, false, "nvenc", "nvenc", false),
    next_probe_reuse(false, false, false, "nvenc", "nvenc", true),
  };
  for (const auto text : refused) {
    EXPECT_EQ(text.rfind("off (", 0), 0u) << text;
    EXPECT_EQ(text.find("allowed"), npos) << text;
  }
  EXPECT_NE(refused[0].find("Vulkan Video"), npos) << refused[0];
  EXPECT_NE(refused[1].find("Vulkan Video"), npos) << refused[1];
  EXPECT_NE(refused[2].find("last resort"), npos) << refused[2];
  EXPECT_NE(refused[3].find("Auto policy"), npos) << refused[3];
  EXPECT_NE(refused[4].find("a fresh probe runs every time here"), npos) << refused[4];
  EXPECT_NE(refused[5].find("a fresh probe runs every time here"), npos) << refused[5];
  EXPECT_NE(refused[6].find("left no identity"), npos) << refused[6];
  for (const auto text : {next_probe_reuse(true, false, false, "", "nvenc", true), refused[0], refused[1],
                          refused[2], refused[3], refused[4], refused[6]}) {
    for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
      EXPECT_EQ(text.find(dash), npos) << text;
    }
  }
}

TEST(VideoProbeReuseTests, IdentityChangeDuringProbeAndFailureInvalidatePriorSuccess) {
  video::probe_reuse::cache_t cache;
  const auto before = complete_probe_identity();
  auto after = before;
  after.topology = "new-compositor-generation";
  cache.remember(before, before, "nvenc", cache.epoch());
  cache.remember(before, after, "nvenc", cache.epoch());
  EXPECT_FALSE(cache.reusable(before, "nvenc", "", false));
  EXPECT_FALSE(cache.reusable(after, "nvenc", "", false));
  cache.remember(before, before, "nvenc", cache.epoch());
  const auto epoch = cache.epoch();
  cache.invalidate();
  EXPECT_FALSE(cache.reusable(before, "nvenc", "", false));
  cache.remember(before, before, "nvenc", epoch);
  EXPECT_FALSE(cache.reusable(before, "nvenc", "", false));
}

TEST(VideoProbeReuseTests, CaptureFailureDoesNotNeedAnEncoderWriterLock) {
  video::probe_reuse::cache_t cache;
  const auto identity = complete_probe_identity();
  cache.remember(identity, identity, "nvenc", cache.epoch());
  std::thread capture([&] { for (int i = 0; i < 10000; ++i) cache.invalidate(); });
  for (int i = 0; i < 10000; ++i) (void) cache.reusable(identity, "nvenc", "", false);
  capture.join();
  EXPECT_FALSE(cache.reusable(identity, "nvenc", "", false));
}

TEST(VideoProbeReuseTests, PublishedVaapiSettingsInvalidateIdentityWithoutMutatingStartupConfig) {
  const auto saved = config::vaapi::snapshot();
  auto restore = util::fail_guard([&] { config::vaapi::publish(saved); });
  const config::video_t startup {};
  config::vaapi::publish({});
  const auto automatic = video::encoder_probe_settings_for_tests(startup);
  auto check = [&](const config::vaapi::settings_t &settings) {
    config::vaapi::publish(settings);
    EXPECT_NE(video::encoder_probe_settings_for_tests(startup), automatic);
    config::vaapi::publish({});
    EXPECT_EQ(video::encoder_probe_settings_for_tests(startup), automatic);
  };
  check({.strict_rc_buffer = true});
  check({.quality = config::vaapi::quality_e::speed});
  check({.rc = config::vaapi::rc_e::cqp});
  check({.blbrc = true});
  check({.blbrc = false});
}

TEST(VideoProbeReuseTests, CapabilitySettingsRetireReuseAcrossEncoderFamilies) {
  const config::video_t original {};
  const auto key = video::encoder_probe_settings_for_tests(original);
  auto changed = original;
  auto check = [&] {
    EXPECT_NE(video::encoder_probe_settings_for_tests(changed), key);
    changed = original;
  };
  changed.encoder = "nvenc"; check();
  changed.adapter_name = "/dev/dri/renderD129"; check();
  changed.output_name = "second-output"; check();
  changed.hevc_mode = 3; check();
  changed.color_range = 2; check();
  changed.sw.sw_preset = "medium"; check();
  changed.nv.quality_preset = 7; check();
  changed.nv_legacy.multipass = 2; check();
  changed.qsv.qsv_preset = 4; check();
  changed.amd.amd_quality_h264 = 2; check();
  changed.vt.vt_allow_sw = 1; check();
  changed.vaapi.strict_rc_buffer = true; check();
  changed.vk.rc_mode = 1; check();
  changed.linux_display.prefer_gpu_native_capture = true; check();
  // Unrelated credentials never enter process or disk probe provenance.
  changed.ai_optimizer.api_key = "unrelated-secret";
  EXPECT_EQ(video::encoder_probe_settings_for_tests(changed), key);
}
#endif

#if defined(POLARIS_TESTS) && !defined(__APPLE__)
TEST(VideoProbeReuseTests, ResetTimeoutDuringRealProbeEntryCannotAuthorizeReuse) {
  const auto old_config = config::video;
  const auto old_h264 = video::nvenc.h264.capabilities;
  const auto old_hevc = video::nvenc.hevc.capabilities;
  const auto old_av1 = video::nvenc.av1.capabilities;
  auto restore = util::fail_guard([&] {
    video::reset_encoder_probe_state();
    config::video = old_config;
    video::nvenc.h264.capabilities = old_h264;
    video::nvenc.hevc.capabilities = old_hevc;
    video::nvenc.av1.capabilities = old_av1;
  });
  video::reset_encoder_probe_state();
  config::video.encoder = "nvenc";
  config::video.hevc_mode = 1;
  config::video.av1_mode = 1;
#ifdef __linux__
  // The test identity stands in for providers and hardware, not for the route, so this test sits
  // on the one route where the real gate can reuse a probe: NVENC on the labwc private compositor.
  config::video.linux_display.use_cage_compositor = true;
#endif
  const auto identity = complete_probe_identity();
  std::promise<void> entered;
  std::promise<void> resume;
  auto resumed = resume.get_future();
  auto validate = [](video::encoder_t &encoder, bool) {
    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();
    return true;
  };
  auto probe = std::async(std::launch::async, [&] {
    return video::probe_encoders_with_hooks_for_tests(identity, [&](video::encoder_t &encoder, bool expected) {
      entered.set_value();
      resumed.wait();
      return validate(encoder, expected);
    });
  });
  entered.get_future().wait();
  // This is the actual reset API and actual encoder-state mutex: the writer
  // remains in its probe while reset invalidates, waits two seconds, and defers.
  video::reset_encoder_probe_state();
  resume.set_value();
  EXPECT_EQ(probe.get(), 0);
  int validations = 0;
  auto count = [&](video::encoder_t &encoder, bool expected) {
    ++validations;
    return validate(encoder, expected);
  };
  EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(identity, count), 0);
  EXPECT_EQ(validations, 1) << "timed-out external reset must force another real validation";
  EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(identity, count), 0);
  EXPECT_EQ(validations, 1) << "unchanged successful validation may then reuse";
}
#endif

#if defined(POLARIS_TESTS) && defined(__linux__)
namespace {
  /// The lines the host logs while it is alive, as an operator reads them.
  class ProbeLogCapture {
  public:
    ProbeLogCapture():
        stream_ {boost::make_shared<std::ostringstream>()} {
      auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
      backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
      backend->auto_flush(true);
      sink_ = boost::make_shared<sink_t>(backend);
      sink_->set_formatter(&logging::formatter);
      boost::log::core::get()->add_sink(sink_);
    }

    ~ProbeLogCapture() {
      boost::log::core::get()->remove_sink(sink_);
    }

    ProbeLogCapture(const ProbeLogCapture &) = delete;
    ProbeLogCapture &operator=(const ProbeLogCapture &) = delete;

    /// Every captured line that holds needle.
    [[nodiscard]] std::string lines_with(std::string_view needle) const {
      std::istringstream input {stream_->str()};
      std::string out;
      for (std::string line; std::getline(input, line);) {
        if (line.find(needle) != std::string::npos) {
          out += line;
          out += '\n';
        }
      }
      return out;
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    boost::shared_ptr<std::ostringstream> stream_;
    boost::shared_ptr<sink_t> sink_;
  };

  struct next_probe_route_t {
    std::string_view name;
    std::string_view configured;  ///< config::video.encoder; empty is Auto
    std::string_view chosen;  ///< the one encoder the test lets pass validation
    bool private_compositor;
    std::string_view stream_mode;
    std::string_view capture;
    bool gate_reuses;  ///< what the real gate does with the next probe
    std::string_view says;  ///< what the encoder_auto line says about it
  };

  /// Probes twice through the real probe_encoders() and its reuse gate, and returns the
  /// encoder_auto line of the first probe and whether the second one validated nothing.
  std::pair<std::string, bool> probe_twice(const next_probe_route_t &route) {
    video::reset_encoder_probe_state();
    config::video.encoder = std::string {route.configured};
    config::video.hevc_mode = 1;
    config::video.av1_mode = 1;
    config::video.capture = std::string {route.capture};
    config::video.linux_display.use_cage_compositor = route.private_compositor;
    config::video.linux_display.stream_mode = std::string {route.stream_mode};
    int validations = 0;
    auto validate = [&](video::encoder_t &encoder, bool) {
      ++validations;
      return encoder.name == route.chosen;
    };
    const video::probe_reuse::identity_t identity {"pci-gpu-device", "kernel-and-userspace-driver", "live-capture-generation", "capability-settings"};
    ProbeLogCapture log;
    EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(identity, validate), 0) << route.name;
    const auto line = log.lines_with("encoder_auto:");
    const auto first = validations;
    EXPECT_GT(first, 0) << route.name;
    EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(identity, validate), 0) << route.name;
    return {line, validations == first};
  }
}  // namespace

TEST(VideoProbeReuseTests, EncoderAutoLineMatchesWhatTheRealGateDoesWithTheNextProbe) {
  // #635: the line said next_probe_reuse=allowed on routes where the gate never reuses, because it
  // asked only live_probe_mandatory and not whether the route gives a probe an identity at all.
  // Each route here is probed twice through the real gate, and the line of the first probe has to
  // say what the gate then did with the second.
  const auto old_config = config::video;
  auto restore = util::fail_guard([&] {
    video::reset_encoder_probe_state();
    config::video = old_config;
  });
  constexpr auto npos = std::string::npos;
  std::vector<next_probe_route_t> routes {
    {"Auto VA-API on Gamescope Stream", "", "vaapi", false, "gamescope_stream", "portal", false,
     "next_probe_reuse=off (a fresh probe runs every time here"},
    {"NVENC outside labwc", "nvenc", "nvenc", false, "desktop_display", "", false,
     "next_probe_reuse=off (a fresh probe runs every time here"},
    {"NVENC on labwc", "nvenc", "nvenc", true, "headless_stream", "", true,
     "next_probe_reuse=if_unchanged ("},
  };
#ifdef POLARIS_BUILD_VULKAN
  routes.push_back({"explicit Vulkan on Gamescope Stream", "vulkan", "vulkan", false, "gamescope_stream", "portal",
                    false, "next_probe_reuse=off (Vulkan Video probes fresh every time)"});
  routes.push_back({"explicit Vulkan on labwc", "vulkan", "vulkan", true, "headless_stream", "", false,
                    "next_probe_reuse=off (Vulkan Video probes fresh every time)"});
#endif
  for (const auto &route : routes) {
    const auto [line, reused] = probe_twice(route);
    ASSERT_NE(line.find("selected=" + std::string {route.chosen}), npos) << route.name << "\n" << line;
    EXPECT_EQ(reused, route.gate_reuses) << route.name;
    EXPECT_EQ(line.find("next_probe_reuse=if_unchanged") != npos, reused) << route.name << "\n" << line;
    EXPECT_NE(line.find(route.says), npos) << route.name << "\n" << line;
    EXPECT_EQ(line.find("allowed"), npos) << route.name << "\n" << line;
  }
}

#ifdef POLARIS_BUILD_VULKAN
TEST(VideoProbeReuseTests, ExplicitVulkanReasonSaysWhatItCostsHere) {
  // #635 was reported on encoder = vulkan, where the reason said only that the encoder passed
  // validation. It now says what the Auto sentence says Vulkan Video costs, qualified the same way,
  // and opens with Vulkan Video and its first cost, which the Selection reason on the web console's
  // Troubleshooting page and the system stats route show first. Nova's Doctor card never shows it:
  // encoder = vulkan cannot fall back, so this evidence is always graded pass.
  const auto old_config = config::video;
  auto restore = util::fail_guard([&] {
    video::reset_encoder_probe_state();
    config::video = old_config;
  });
  constexpr auto npos = std::string::npos;
  const auto reason_after_probe = [](std::string_view encoder) {
    probe_twice({encoder, encoder, encoder, false, "gamescope_stream", "portal", false, ""});
    return video::active_encoder_selection_info().reason;
  };
  const auto vulkan = reason_after_probe("vulkan");
  EXPECT_EQ(
    vulkan.rfind("Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 here;", 0),
    0u
  ) << vulkan;
  EXPECT_NE(vulkan.find("on portal capture, which Gamescope Stream uses, frames reach the encoder through system memory."), npos) << vulkan;
  const auto nvenc = reason_after_probe("nvenc");
  EXPECT_EQ(nvenc, "The explicitly configured encoder passed runtime validation.");
}

namespace {
  /// A stream mode's state as the load or apply_selection() writes it, and the host's codec settings.
  struct auto_route_t {
    std::string_view name;
    std::string_view driver;  ///< the kernel driver Auto plans for
    std::string_view stream_mode;
    std::string_view private_runtime;
    bool use_cage_compositor = false;
    int hevc_mode = 0;
    int av1_mode = 0;
    std::string_view capture = "portal";  ///< the capture setting off labwc; labwc leaves it unset
  };

  struct auto_probe_t {
    std::vector<std::string> tried;  ///< every encoder the probe validated, in order
    video::encoder_selection_info_t selection;
    video::codec_capability_state_t advertised;
    std::string log;
  };

  /// Every encoder fails its probe.
  constexpr std::string_view k_every_encoder = "*";

  /// Each encoder's validation as an AMD host's probe finds it: H.264, HEVC and Main10 for every
  /// encoder, and AV1 for every one but Vulkan Video (NO_AV1). Records each encoder it validates in
  /// tried, and fails the one named failing, or every one for k_every_encoder.
  std::function<bool(video::encoder_t &, bool)> amd_validate(std::vector<std::string> &tried, std::string_view failing = {}) {
    return [&tried, failing = std::string {failing}](video::encoder_t &encoder, bool) {
      tried.emplace_back(encoder.name);
      if (failing == k_every_encoder || encoder.name == failing) {
        return false;
      }
      encoder.h264.capabilities.set();
      encoder.hevc.capabilities.set();
      encoder.av1.capabilities.set();
      if (encoder.name == "vulkan") {
        encoder.av1.capabilities.reset();
      }
      return true;
    };
  }

  const video::probe_reuse::identity_t k_probe_identity {"pci-gpu-device", "kernel-and-userspace-driver", "live-capture-generation", "capability-settings"};

  /// One Auto probe through the real probe_encoders(), with the GPU driver and each encoder's
  /// validation stood in by amd_validate().
  auto_probe_t probe_auto(const auto_route_t &route, std::string_view failing = {}, std::string_view encoder = {}) {
    video::reset_encoder_probe_state();
    config::video.encoder = std::string {encoder};
    config::video.hevc_mode = route.hevc_mode;
    config::video.av1_mode = route.av1_mode;
    config::video.capture = route.use_cage_compositor ? std::string {} : std::string {route.capture};
    config::video.linux_display.stream_mode = std::string {route.stream_mode};
    config::video.linux_display.private_runtime = std::string {route.private_runtime};
    config::video.linux_display.use_cage_compositor = route.use_cage_compositor;
    auto_probe_t out;
    const auto validate = amd_validate(out.tried, failing);
    ProbeLogCapture log;
    EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(k_probe_identity, validate, route.driver), 0) << route.name;
    out.selection = video::active_encoder_selection_info();
    out.advertised = {video::active_hevc_mode, video::active_av1_mode};
    out.log = log.lines_with("encoder_auto:");
    return out;
  }

  bool tried(const std::vector<std::string> &encoders, std::string_view encoder) {
    return std::find(encoders.begin(), encoders.end(), encoder) != encoders.end();
  }

  bool tried(const auto_probe_t &probe, std::string_view encoder) {
    return tried(probe.tried, encoder);
  }

  struct auto_refresh_t {
    video::auto_plan_refresh_e result;
    std::vector<std::string> tried;  ///< every encoder the refresh probed, in order
    video::encoder_selection_info_t selection;
    video::codec_capability_state_t advertised;
  };

  /// One refresh_advertised_codecs_for_auto_plan() on an AMD host, from the host state as it stands.
  auto_refresh_t refresh_amd_auto_plan(bool stream_active = false, std::string_view failing = {}) {
    auto_refresh_t out;
    const auto validate = amd_validate(out.tried, failing);
    out.result = video::refresh_advertised_codecs_for_auto_plan_with_hooks_for_tests(k_probe_identity, validate, "amdgpu", stream_active);
    out.selection = video::active_encoder_selection_info();
    out.advertised = {video::active_hevc_mode, video::active_av1_mode};
    return out;
  }

  /// Drops a Steam Game Mode hold a test took, however the test ends.
  struct game_mode_hold_guard_t {
    ~game_mode_hold_guard_t() {
      stream_display_policy::forget_game_mode_hold();
    }
  };

  /// Puts back what probe_auto() changes, and every encoder's capability bits.
  struct auto_probe_guard_t {
    auto_probe_guard_t():
        config {config::video},
        vulkan {video::vulkan.h264.capabilities, video::vulkan.hevc.capabilities, video::vulkan.av1.capabilities},
        vaapi {video::vaapi.h264.capabilities, video::vaapi.hevc.capabilities, video::vaapi.av1.capabilities},
        nvenc {video::nvenc.h264.capabilities, video::nvenc.hevc.capabilities, video::nvenc.av1.capabilities} {
    }

    ~auto_probe_guard_t() {
      video::reset_encoder_probe_state();
      config::video = config;
      restore(video::vulkan, vulkan);
      restore(video::vaapi, vaapi);
      restore(video::nvenc, nvenc);
    }

    using caps_t = decltype(video::vulkan.h264.capabilities);
    struct codec_caps_t {
      caps_t h264;
      caps_t hevc;
      caps_t av1;
    };

    static void restore(video::encoder_t &encoder, const codec_caps_t &caps) {
      encoder.h264.capabilities = caps.h264;
      encoder.hevc.capabilities = caps.hevc;
      encoder.av1.capabilities = caps.av1;
    }

    config::video_t config;
    codec_caps_t vulkan;
    codec_caps_t vaapi;
    codec_caps_t nvenc;
  };

  constexpr auto_route_t amd_gamescope_stream {"AMD Gamescope Stream", "amdgpu", "gamescope_stream", "gamescope"};
}  // namespace

TEST(VideoAutoPolicyProbeTests, AmdGamescopeStreamTriesVulkanFirstAndOffersNeitherAv1NorHdr) {
  // #635, option (a). Vulkan Video was erased from Auto's candidates on this route, so it could
  // never be tried. It is now tried first, and the host offers what it can serve through the
  // system memory upload: HEVC Main, no Main10 and no AV1.
  const auto_probe_guard_t guard;
  const auto probe = probe_auto(amd_gamescope_stream);

  ASSERT_FALSE(probe.tried.empty());
  EXPECT_EQ(probe.tried.front(), "vulkan");
  EXPECT_EQ(probe.selection.policy, "amd_gamescope_vulkan_ram");
  EXPECT_EQ(probe.selection.preferred_encoder, "vulkan");
  EXPECT_EQ(probe.selection.fallback_encoder, "vaapi");
  EXPECT_EQ(probe.selection.selected_encoder, "vulkan");
  EXPECT_FALSE(probe.selection.fallback_used);
  EXPECT_FALSE(probe.selection.exact_live_probe_required);
  EXPECT_EQ(probe.advertised.hevc_mode, 2) << "HEVC Main without Main10: the upload reads 8-bit frames only";
  EXPECT_EQ(probe.advertised.av1_mode, 1) << "Vulkan Video carries no AV1";
  EXPECT_EQ(
    probe.selection.reason.rfind("Auto prefers Vulkan Video on AMD Gamescope Stream, through system memory;", 0),
    0u
  ) << probe.selection.reason;
  EXPECT_NE(probe.log.find("policy=amd_gamescope_vulkan_ram preferred=vulkan selected=vulkan fallback_used=false"), std::string::npos)
    << probe.log;
  EXPECT_NE(probe.log.find("so this host offers no HDR with it"), std::string::npos) << probe.log;
}

TEST(VideoAutoPolicyProbeTests, AFailedVulkanProbeOnGamescopeStreamFallsBackToVaapi) {
  // Strict semantics belong to encoder = vulkan. Under Auto a Vulkan Video probe that fails hands
  // the stream to VA-API, which keeps AV1 and the HDR profile it passed.
  const auto_probe_guard_t guard;
  const auto probe = probe_auto(amd_gamescope_stream, "vulkan");

  ASSERT_GE(probe.tried.size(), 2u);
  EXPECT_EQ(probe.tried[0], "vulkan");
  EXPECT_EQ(probe.tried[1], "vaapi");
  EXPECT_EQ(probe.selection.selected_encoder, "vaapi");
  EXPECT_TRUE(probe.selection.fallback_used);
  EXPECT_EQ(
    probe.selection.reason.rfind("Preferred encoder [vulkan] did not satisfy this runtime; selected [vaapi] instead.", 0),
    0u
  ) << probe.selection.reason;
  EXPECT_EQ(probe.advertised.hevc_mode, 3);
  EXPECT_EQ(probe.advertised.av1_mode, 3);
  EXPECT_EQ(probe.log.find("offers no HDR"), std::string::npos) << probe.log;
}

TEST(VideoAutoPolicyProbeTests, AskingForAv1OrHdrKeepsVaapiOnGamescopeStreamWithoutProbingVulkan) {
  // AV1 Support set to always advertise AV1 must end on VA-API: Vulkan Video carries no AV1, so the
  // requirement loop would pass over it, but only after a probe, and the reason would call a host
  // setting a failure. The policy keeps VA-API before any probe, and so does asking for HDR.
  const auto_probe_guard_t guard;
  struct case_t {
    int hevc_mode;
    int av1_mode;
  };
  for (const auto [hevc_mode, av1_mode] : {case_t {0, 2}, case_t {0, 3}, case_t {3, 0}}) {
    auto route = amd_gamescope_stream;
    route.hevc_mode = hevc_mode;
    route.av1_mode = av1_mode;
    const auto probe = probe_auto(route);
    const auto label = "hevc_mode=" + std::to_string(hevc_mode) + " av1_mode=" + std::to_string(av1_mode);

    EXPECT_FALSE(tried(probe, "vulkan")) << label;
    EXPECT_EQ(probe.selection.policy, "amd_gamescope_vaapi_codec_setting") << label;
    EXPECT_EQ(probe.selection.selected_encoder, "vaapi") << label;
    EXPECT_FALSE(probe.selection.fallback_used) << label;
    if (av1_mode >= 2) {
      EXPECT_EQ(probe.advertised.av1_mode, av1_mode) << label;
    }
    if (hevc_mode == 3) {
      EXPECT_EQ(probe.advertised.hevc_mode, 3) << label;
    }
  }

  // AV1 Support on its default is the trade papi chose: Vulkan Video, and no AV1.
  const auto default_av1 = probe_auto(amd_gamescope_stream);
  EXPECT_EQ(default_av1.selection.selected_encoder, "vulkan");
  EXPECT_EQ(default_av1.advertised.av1_mode, 1);
}

TEST(VideoAutoPolicyProbeTests, EveryOtherRouteAndDriverKeepsItsAutoChoice) {
  const auto_probe_guard_t guard;
  struct expected_t {
    auto_route_t route;
    std::string_view policy;
    std::string_view selected;
    bool vulkan_tried;
    int hevc_mode;
  };
  const std::vector<expected_t> cases {
    // labwc keeps its live-frame policy, and its HDR: the HDR rule belongs to Gamescope Stream.
    {{"AMD Private Stream on labwc", "amdgpu", "headless_stream", "labwc", true}, "amd_private_vulkan_live_probe", "vulkan", true, 3},
    {{"AMD Mirror Desktop", "amdgpu", "desktop_display", ""}, "amd_established_desktop", "vaapi", false, 3},
    // The dongle fills an unset capture with the portal, as Gamescope Stream does, and is still not
    // the Gamescope Stream route: it writes no private runtime.
    {{"AMD Headless Dongle, capture unset", "amdgpu", "headless_dongle", "", false, 0, 0, ""}, "amd_established_desktop", "vaapi", false, 3},
    // Steam Game Mode's screen is not a row: SteamGameModeProbesAgainWhenItTakesGamescopeStreamAndWhenItGivesItBack
    // takes the real hold and probes through it.
    {{"NVIDIA Gamescope Stream", "nvidia", "gamescope_stream", "gamescope"}, "nvidia_nvenc", "nvenc", false, 3},
    {{"Intel Gamescope Stream", "xe", "gamescope_stream", "gamescope"}, "intel_vaapi", "vaapi", false, 3},
  };
  for (const auto &expected : cases) {
    const auto probe = probe_auto(expected.route);
    EXPECT_EQ(probe.selection.policy, expected.policy) << expected.route.name;
    EXPECT_EQ(probe.selection.selected_encoder, expected.selected) << expected.route.name;
    EXPECT_FALSE(probe.selection.fallback_used) << expected.route.name;
    EXPECT_EQ(tried(probe, "vulkan"), expected.vulkan_tried) << expected.route.name;
    EXPECT_EQ(probe.advertised.hevc_mode, expected.hevc_mode) << expected.route.name;
    EXPECT_EQ(probe.log.find("offers no HDR"), std::string::npos) << expected.route.name;
  }
}

TEST(VideoAutoPolicyProbeTests, GamescopeStreamOnKmsWlrOrX11CaptureKeepsVaapi) {
  // Gamescope Stream keeps a configured kms, wlr or x11 capture, and those hand Vulkan Video GPU
  // frames that nothing retires, so Auto stays on VA-API there. Unset fills to the portal, and so
  // does auto, which the settings file loads as unset.
  const auto_probe_guard_t guard;
  for (const auto capture : {"kms", "wlr", "x11"}) {
    auto route = amd_gamescope_stream;
    route.capture = capture;
    const auto probe = probe_auto(route);
    EXPECT_EQ(probe.selection.policy, "amd_established_desktop") << capture;
    EXPECT_EQ(probe.selection.selected_encoder, "vaapi") << capture;
    EXPECT_FALSE(tried(probe, "vulkan")) << capture;
    EXPECT_EQ(probe.advertised.av1_mode, 3) << capture;
    // Told it is on Gamescope Stream off the portal, never outside Gamescope Stream.
    EXPECT_EQ(probe.selection.reason.find("outside labwc and Gamescope Stream;"), std::string::npos) << probe.selection.reason;
    EXPECT_NE(
      probe.selection.reason.find("Gamescope Stream with capture set to kms, wlr or x11 stays on VA-API."),
      std::string::npos
    ) << probe.selection.reason;
  }
  std::unordered_map<std::string, std::string> autodetect_vars {{"capture", "auto"}};
  const auto autodetect = config::capture_setting(autodetect_vars, {});
  for (const std::string_view capture : {std::string_view {}, std::string_view {"kwin"}, std::string_view {autodetect}}) {
    auto route = amd_gamescope_stream;
    route.capture = capture;
    const auto probe = probe_auto(route);
    EXPECT_EQ(probe.selection.policy, "amd_gamescope_vulkan_ram") << capture;
    EXPECT_EQ(probe.selection.selected_encoder, "vulkan") << capture;
  }
}

TEST(VideoAutoPolicyProbeTests, SteamGameModeProbesAgainWhenItTakesGamescopeStreamAndWhenItGivesItBack) {
  // The hold swaps Gamescope Stream for Mirror Desktop, and back, with no launch to probe either
  // change. Without the refresh the host advertised Vulkan Video's codecs all through Game Mode,
  // and VA-API's AV1 and Main10 after it, where the next launch probed Vulkan Video and refused an
  // AV1 client at ANNOUNCE and an HDR launch with a 503. A host that starts in Game Mode is the first
  // half: its startup probe planned for Gamescope Stream, before anything took the hold.
  const auto_probe_guard_t guard;
  const game_mode_hold_guard_t hold;
  using stream_display_policy::game_mode_reconcile_e;

  const auto startup = probe_auto(amd_gamescope_stream);
  ASSERT_EQ(startup.selection.policy, "amd_gamescope_vulkan_ram");
  ASSERT_EQ(startup.advertised.hevc_mode, 2);
  ASSERT_EQ(startup.advertised.av1_mode, 1);
  EXPECT_EQ(refresh_amd_auto_plan().result, video::auto_plan_refresh_e::current) << "nothing has moved yet";

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  const auto in_game_mode = refresh_amd_auto_plan();
  EXPECT_EQ(in_game_mode.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_FALSE(tried(in_game_mode.tried, "vulkan"));
  EXPECT_EQ(in_game_mode.selection.policy, "amd_established_desktop");
  EXPECT_EQ(in_game_mode.selection.selected_encoder, "vaapi");
  EXPECT_EQ(in_game_mode.advertised.hevc_mode, 3);
  EXPECT_EQ(in_game_mode.advertised.av1_mode, 3);

  // One probe per change: the next request finds the codecs current and probes nothing.
  const auto polled = refresh_amd_auto_plan();
  EXPECT_EQ(polled.result, video::auto_plan_refresh_e::current);
  EXPECT_TRUE(polled.tried.empty());

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  const auto after_game_mode = refresh_amd_auto_plan();
  EXPECT_EQ(after_game_mode.result, video::auto_plan_refresh_e::reprobed);
  ASSERT_FALSE(after_game_mode.tried.empty());
  EXPECT_EQ(after_game_mode.tried.front(), "vulkan");
  EXPECT_EQ(after_game_mode.selection.policy, "amd_gamescope_vulkan_ram");
  EXPECT_EQ(after_game_mode.selection.selected_encoder, "vulkan");
  EXPECT_EQ(after_game_mode.advertised.hevc_mode, 2);
  EXPECT_EQ(after_game_mode.advertised.av1_mode, 1);
}

TEST(VideoAutoPolicyProbeTests, TheHostDefaultAfterAGamescopeStreamLaunchAdvertisesItsOwnCodecs) {
  // A launch that switched Mirror Desktop to Gamescope Stream for itself probed Vulkan Video, and
  // teardown handed the default back without a probe. The next Mirror Desktop launch was resolved
  // SDR and without AV1, from Vulkan Video's codecs, although its own probe then chose VA-API.
  const auto_probe_guard_t guard;
  const auto session = probe_auto(amd_gamescope_stream);
  ASSERT_EQ(session.selection.selected_encoder, "vulkan");

  // Teardown restores the host default.
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.linux_display.private_runtime.clear();

  // While a stream or an app still runs, its encoder stays.
  const auto running = refresh_amd_auto_plan(true);
  EXPECT_EQ(running.result, video::auto_plan_refresh_e::deferred);
  EXPECT_TRUE(running.tried.empty());
  EXPECT_EQ(running.selection.selected_encoder, "vulkan");

  const auto idle = refresh_amd_auto_plan(false);
  EXPECT_EQ(idle.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_EQ(idle.selection.policy, "amd_established_desktop");
  EXPECT_EQ(idle.selection.selected_encoder, "vaapi");
  EXPECT_EQ(idle.advertised.hevc_mode, 3);
  EXPECT_EQ(idle.advertised.av1_mode, 3);
}

TEST(VideoAutoPolicyProbeTests, ARefreshWhoseProbeFailsKeepsTheEncoderItHadAndIsNotRunAgain) {
  // Nova polls serverinfo, and every poll asks for the refresh; a probe that fails must not run again
  // on each one. It must not take the working encoder with it either. The failed probe used to leave
  // no encoder at all: serverinfo offered H.264 alone, the Doctor showed no encoder, and the next
  // request found no encoder to compare, so it never read the failed change and never refreshed.
  const auto_probe_guard_t guard;
  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.linux_display.private_runtime.clear();

  const auto failed = refresh_amd_auto_plan(false, k_every_encoder);
  EXPECT_EQ(failed.result, video::auto_plan_refresh_e::failed);
  EXPECT_FALSE(failed.tried.empty());
  EXPECT_EQ(failed.selection.selected_encoder, "vulkan");
  EXPECT_EQ(failed.selection.policy, "amd_gamescope_vulkan_ram");
  EXPECT_EQ(failed.advertised.hevc_mode, 2);
  EXPECT_EQ(failed.advertised.av1_mode, 1);
  EXPECT_EQ(video::active_encoder_name(), "vulkan");

  const auto next = refresh_amd_auto_plan(false, k_every_encoder);
  EXPECT_EQ(next.result, video::auto_plan_refresh_e::failed);
  EXPECT_TRUE(next.tried.empty());
  EXPECT_EQ(next.selection.selected_encoder, "vulkan");
}

TEST(VideoAutoPolicyProbeTests, AFailedRefreshIsTriedAgainOnceTheEncoderOrThePlanMoves) {
  // The failed change is remembered so a failing probe does not run on every poll, and it belongs to
  // the encoder that probe failed to replace. It outlived that encoder: a launch probe, or the plan
  // coming back to where the encoder was probed, left it in place, and the next time the plan made
  // the same move the host answered from it without probing and kept the other encoder's codecs.
  // Nothing reset it between tests either, so these tests passed only in the order they are written.
  const auto_probe_guard_t guard;
  const auto to_mirror_desktop = [] {
    config::video.linux_display.stream_mode = "desktop_display";
    config::video.linux_display.private_runtime.clear();
  };

  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  to_mirror_desktop();
  ASSERT_EQ(refresh_amd_auto_plan(false, k_every_encoder).result, video::auto_plan_refresh_e::failed);

  // The plan comes back to Gamescope Stream before a request probes it, then moves again.
  config::video.linux_display.stream_mode = "gamescope_stream";
  config::video.linux_display.private_runtime = "gamescope";
  EXPECT_EQ(refresh_amd_auto_plan().result, video::auto_plan_refresh_e::current);
  to_mirror_desktop();
  const auto moved_again = refresh_amd_auto_plan();
  EXPECT_EQ(moved_again.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_EQ(moved_again.selection.selected_encoder, "vaapi");

  // A launch probe replaces the encoder a failed refresh left.
  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  to_mirror_desktop();
  ASSERT_EQ(refresh_amd_auto_plan(false, k_every_encoder).result, video::auto_plan_refresh_e::failed);
  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  to_mirror_desktop();
  const auto after_launch = refresh_amd_auto_plan();
  EXPECT_EQ(after_launch.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_EQ(after_launch.selection.selected_encoder, "vaapi");
}

TEST(VideoAutoPolicyProbeTests, AnHdrLaunchOnGamescopeStreamIsRefusedForHdrWithoutPromisingItFromVaapi) {
  // A launch that switches to Gamescope Stream for itself was offered HDR by the encoder of the mode
  // it came from. Vulkan Video then passed its probe, and the launch was refused as "No video
  // encoder could start", which sent the player looking for a fault that was not there. The advice
  // must not promise HDR from VA-API either: its packed RGB10 portal upload preserves the
  // negotiated words, but physical HDR capture and playback remain unvalidated.
  const auto_probe_guard_t guard;
  launch_failure::clear();

  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  EXPECT_TRUE(video::active_encoder_withholds_hdr());
  video::note_launch_refused_for_hdr();
  auto refusal = launch_failure::take();
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 503);
  EXPECT_EQ(refusal->code, "encoder_offers_no_hdr");
  EXPECT_EQ(refusal->message.find("No video encoder could start"), std::string::npos) << refusal->message;
  EXPECT_NE(refusal->message.find("on Gamescope Stream Auto encodes with Vulkan Video, which offers no HDR there"), std::string::npos)
    << refusal->message;
  EXPECT_NE(refusal->action.find("Launch without HDR."), std::string::npos) << refusal->action;
  EXPECT_NE(refusal->action.find("hevc_mode = 3 or encoder = vaapi keeps VA-API on Gamescope Stream."), std::string::npos)
    << refusal->action;
  EXPECT_NE(refusal->action.find("VA-API can upload negotiated packed RGB10"), std::string::npos) << refusal->action;
  EXPECT_NE(refusal->action.find("HDR capture and playback on this route are not proven"), std::string::npos) << refusal->action;
  EXPECT_EQ(refusal->action.find("same 8-bit system memory upload"), std::string::npos) << refusal->action;
  EXPECT_EQ(refusal->action.find("POLARIS_PORTAL_DMABUF"), std::string::npos) << refusal->action;
  EXPECT_EQ(refusal->action.find("For HDR on Gamescope Stream"), std::string::npos) << refusal->action;
  for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
    EXPECT_EQ(launch_failure::status_message(*refusal).find(dash), std::string::npos);
  }

  // VA-API after a failed Vulkan Video probe, and Vulkan Video on labwc, keep their HDR: neither is
  // withheld by the route, and a refusal names the encoder instead.
  ASSERT_EQ(probe_auto(amd_gamescope_stream, "vulkan").selection.selected_encoder, "vaapi");
  EXPECT_FALSE(video::active_encoder_withholds_hdr());
  video::note_launch_refused_for_hdr();
  refusal = launch_failure::take();
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "encoder_offers_no_hdr");
  EXPECT_NE(refusal->message.find("the encoder this host selected for it, vaapi, offers no HDR profile"), std::string::npos)
    << refusal->message;
  EXPECT_EQ(refusal->message.find("Gamescope Stream"), std::string::npos) << refusal->message;

  ASSERT_EQ(probe_auto({"AMD Private Stream on labwc", "amdgpu", "headless_stream", "labwc", true}).selection.selected_encoder, "vulkan");
  EXPECT_FALSE(video::active_encoder_withholds_hdr());
  launch_failure::clear();
}

TEST(VideoAutoPolicyProbeTests, ExplicitVulkanOnGamescopeStreamOffersNoHdrUnlessHevcSupportAsksForIt) {
  // #635: an explicit encoder = vulkan reads Gamescope Stream frames
  // through the same 8-bit system memory upload as Auto's Vulkan Video, and the host offered Main10
  // with it from a probe that encoded a zeroed 8-bit frame. It now offers no HDR there unless HEVC
  // Support asks for it, which is kept as written. Off Gamescope Stream nothing changes.
  const auto_probe_guard_t guard;
  launch_failure::clear();
  constexpr auto npos = std::string::npos;

  const auto withheld = probe_auto(amd_gamescope_stream, {}, "vulkan");
  ASSERT_EQ(withheld.selection.selected_encoder, "vulkan");
  EXPECT_EQ(withheld.selection.policy, "explicit");
  EXPECT_TRUE(withheld.selection.vulkan_withholds_hdr);
  EXPECT_EQ(withheld.advertised.hevc_mode, 2);
  EXPECT_TRUE(video::active_encoder_withholds_hdr());
  EXPECT_TRUE(video::active_encoder_withholds_hdr_for_explicit_vulkan());
  EXPECT_NE(withheld.selection.reason.find("so on Gamescope Stream this host offers no HDR with it"), npos)
    << withheld.selection.reason;
  video::note_launch_refused_for_hdr();
  auto refusal = launch_failure::take();
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "encoder_offers_no_hdr");
  EXPECT_NE(refusal->message.find("this host is set to encoder = vulkan, which offers no HDR on Gamescope Stream"), npos)
    << refusal->message;
  EXPECT_EQ(refusal->message.find("Auto"), npos) << refusal->message;
  EXPECT_NE(refusal->action.find("Launch without HDR."), npos) << refusal->action;
  // hevc_mode = 3 would offer HDR with Vulkan Video here and the stream would end, so it is no advice.
  EXPECT_EQ(refusal->action.find("hevc_mode = 3"), npos) << refusal->action;

  // Vulkan Video a launch chose for itself sits where polaris.conf's encoder does until teardown, and
  // the host may be on Auto or VA-API, so the refusal names the launch's choice instead.
  video::note_launch_refused_for_hdr(true);
  refusal = launch_failure::take();
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "encoder_offers_no_hdr");
  EXPECT_NE(refusal->message.find("This launch asks for HDR with Vulkan Video chosen for it"), npos) << refusal->message;
  EXPECT_EQ(refusal->message.find("set to encoder = vulkan"), npos) << refusal->message;
  EXPECT_NE(refusal->action.find("choose another encoder for this launch"), npos) << refusal->action;
  EXPECT_EQ(refusal->action.find("encoder = vaapi"), npos) << refusal->action;
  EXPECT_EQ(refusal->action.find("hevc_mode = 3"), npos) << refusal->action;

  // HEVC Support set to advertise HDR is an explicit request, kept as written.
  auto asks_for_hdr = amd_gamescope_stream;
  asks_for_hdr.hevc_mode = 3;
  const auto kept = probe_auto(asks_for_hdr, {}, "vulkan");
  ASSERT_EQ(kept.selection.selected_encoder, "vulkan");
  EXPECT_FALSE(kept.selection.vulkan_withholds_hdr);
  EXPECT_EQ(kept.advertised.hevc_mode, 3);
  // The bit an HDR session is built from survives, not only the setting it was read from.
  EXPECT_TRUE(video::vulkan.hevc[video::encoder_t::DYNAMIC_RANGE]);
  EXPECT_FALSE(video::active_encoder_withholds_hdr());

  // Off Gamescope Stream an explicit Vulkan Video keeps the Main10 its probe passed.
  const auto mirror = probe_auto({"AMD Mirror Desktop", "amdgpu", "desktop_display", ""}, {}, "vulkan");
  ASSERT_EQ(mirror.selection.selected_encoder, "vulkan");
  EXPECT_FALSE(mirror.selection.vulkan_withholds_hdr);
  EXPECT_EQ(mirror.advertised.hevc_mode, 3);
  EXPECT_FALSE(video::active_encoder_withholds_hdr());
  launch_failure::clear();
}

TEST(VideoAutoPolicyProbeTests, ExplicitVulkanProbesAgainWhenTheRouteDecidesItsHdr) {
  // An explicit encoder keeps the policy "explicit" on every route, and the refresh compared plans by
  // policy alone. A host set to encoder = vulkan that left Gamescope Stream would have gone on
  // advertising no HDR, and one that came back would have offered Main10 the route cannot carry.
  const auto_probe_guard_t guard;
  const auto to_mirror_desktop = [] {
    config::video.linux_display.stream_mode = "desktop_display";
    config::video.linux_display.private_runtime.clear();
  };
  const auto to_gamescope_stream = [] {
    config::video.linux_display.stream_mode = "gamescope_stream";
    config::video.linux_display.private_runtime = "gamescope";
  };

  ASSERT_EQ(probe_auto(amd_gamescope_stream, {}, "vulkan").advertised.hevc_mode, 2);
  to_mirror_desktop();
  const auto left = refresh_amd_auto_plan();
  EXPECT_EQ(left.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_EQ(left.selection.selected_encoder, "vulkan");
  EXPECT_EQ(left.advertised.hevc_mode, 3);
  EXPECT_FALSE(video::active_encoder_withholds_hdr());

  to_gamescope_stream();
  const auto back = refresh_amd_auto_plan();
  EXPECT_EQ(back.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_EQ(back.selection.selected_encoder, "vulkan");
  EXPECT_EQ(back.advertised.hevc_mode, 2);
  EXPECT_TRUE(video::active_encoder_withholds_hdr());

  // Nothing that decides its HDR moved, so nothing is probed.
  EXPECT_EQ(refresh_amd_auto_plan().result, video::auto_plan_refresh_e::current);

  // A move to the private compositor drops it for the cage probe, and the log names both plans
  // by their keys, not "explicit -> explicit".
  config::video.linux_display.stream_mode = "headless_stream";
  config::video.linux_display.private_runtime = "labwc";
  config::video.linux_display.use_cage_compositor = true;
  config::video.capture.clear();
  ProbeLogCapture log;
  const auto cage = refresh_amd_auto_plan();
  EXPECT_EQ(cage.result, video::auto_plan_refresh_e::left_to_cage_probe);
  EXPECT_TRUE(video::active_encoder_name().empty());
  const auto moved = log.lines_with("encoder_auto:");
  EXPECT_NE(moved.find("[explicit without Vulkan Video HDR -> explicit]"), std::string::npos) << moved;
  EXPECT_EQ(moved.find("Auto plan"), std::string::npos) << moved;
}

TEST(VideoAutoPolicyProbeTests, AnAv1RefusalAtAnnounceSaysWhatTookAv1Away) {
  // #635: a launch that switches into Gamescope Stream on AMD cannot be
  // refused for AV1 by name, because the client picks its codec at ANNOUNCE, after the launch. The
  // refusal there logged "AV1 is disabled, yet the client requested AV1" whatever took AV1 away.
  const auto_probe_guard_t guard;
  constexpr auto npos = std::string::npos;

  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  EXPECT_EQ(video::active_av1_mode, 1);
  const auto on_gamescope = video::av1_announce_refusal(config::video.av1_mode, video::active_encoder_selection_info());
  EXPECT_NE(on_gamescope.find("on AMD Gamescope Stream Auto encodes with Vulkan Video, which carries no AV1"), npos)
    << on_gamescope;
  EXPECT_NE(on_gamescope.find("av1_mode = 2 keeps VA-API and AV1 on Gamescope Stream."), npos) << on_gamescope;

  // The setting is named when it is the cause, whatever the encoder.
  const auto by_setting = video::av1_announce_refusal(1, video::active_encoder_selection_info());
  EXPECT_NE(by_setting.find("AV1 Support is set to never advertise it (av1_mode = 1)"), npos) << by_setting;
  EXPECT_EQ(by_setting.find("Vulkan Video"), npos) << by_setting;

  // Any other encoder without AV1 is named, without Gamescope Stream or Auto.
  const auto explicit_vulkan = probe_auto({"AMD Mirror Desktop", "amdgpu", "desktop_display", ""}, {}, "vulkan");
  ASSERT_EQ(explicit_vulkan.selection.selected_encoder, "vulkan");
  const auto named = video::av1_announce_refusal(config::video.av1_mode, explicit_vulkan.selection);
  EXPECT_NE(named.find("the encoder this host selected, vulkan, offers no AV1"), npos) << named;
  EXPECT_EQ(named.find("Gamescope"), npos) << named;
  EXPECT_EQ(named.find("Auto"), npos) << named;

  const auto none = video::av1_announce_refusal(0, video::encoder_selection_info_t {});
  EXPECT_NE(none.find("this host advertises no AV1"), npos) << none;

  for (const auto &sentence : {on_gamescope, by_setting, named, none}) {
    EXPECT_EQ(sentence.rfind("The client asked for AV1, and ", 0), 0u) << sentence;
    for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
      EXPECT_EQ(sentence.find(dash), npos) << sentence;
    }
  }
}

TEST(VideoAutoPolicyProbeTests, TheDoctorExplainsAGamescopeStreamHdrRefusalWithoutPromisingHdrFromVaapi) {
  // The resolver's host_encoder_hdr_unsupported comes from this route's policy far more often than
  // from a fallback, and the Doctor told the player to fix an NVENC or VA-API fallback that had not
  // happened, beside an encoder row that read pass. Its advice must not promise HDR from VA-API,
  // which takes the same 8-bit upload on the portal unless the DMA-BUF opt-in is set.
  const auto_probe_guard_t guard;
  stream_stats::stats_t stats {};
  stats.hdr_policy_hdr = false;
  stats.hdr_policy_reason = "host_encoder_hdr_unsupported";
  stats.hdr_policy_device = "RetroidPocket6";
  const auto hdr_warning = [&stats] {
    const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});
    for (const auto &warning : doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
      if (warning.at("id") == "hdr_disabled_by_saved_setting") {
        return warning;
      }
    }
    return nlohmann::json {};
  };

  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  auto warning = hdr_warning();
  ASSERT_FALSE(warning.is_null());
  const auto message = warning.at("message").get<std::string>();
  const auto action = warning.at("action").get<std::string>();
  EXPECT_NE(message.find("Auto encodes Gamescope Stream on this AMD host with Vulkan Video"), std::string::npos) << message;
  EXPECT_NE(message.find("Nothing fell back"), std::string::npos) << message;
  EXPECT_NE(action.find("hevc_mode = 3 or encoder = vaapi keeps VA-API there, but"), std::string::npos) << action;
  EXPECT_NE(action.find("the same 8-bit system memory upload unless POLARIS_PORTAL_DMABUF=1 is set"), std::string::npos) << action;
  EXPECT_NE(action.find("is not proven"), std::string::npos) << action;
  EXPECT_EQ(action.find("which offers Main10"), std::string::npos) << action;
  EXPECT_EQ(action.find("fell back"), std::string::npos) << action;

  // VA-API after a failed Vulkan Video probe keeps the advice that fits a fallback.
  ASSERT_EQ(probe_auto(amd_gamescope_stream, "vulkan").selection.selected_encoder, "vaapi");
  warning = hdr_warning();
  ASSERT_FALSE(warning.is_null());
  EXPECT_NE(warning.at("action").get<std::string>().find("If NVENC or VA-API fell back"), std::string::npos);

  // An explicit encoder = vulkan is named as the setting it is, and hevc_mode = 3 is no advice there.
  ASSERT_EQ(probe_auto(amd_gamescope_stream, {}, "vulkan").selection.selected_encoder, "vulkan");
  warning = hdr_warning();
  ASSERT_FALSE(warning.is_null());
  const auto explicit_message = warning.at("message").get<std::string>();
  const auto explicit_action = warning.at("action").get<std::string>();
  // Stream stats carry no flag for an encoder the launch chose, so the Doctor names both sources.
  EXPECT_NE(explicit_message.find("this stream encodes with Vulkan Video, set by encoder = vulkan or chosen for the launch"),
            std::string::npos)
    << explicit_message;
  EXPECT_EQ(explicit_message.find("Auto"), std::string::npos) << explicit_message;
  EXPECT_EQ(explicit_action.find("hevc_mode = 3"), std::string::npos) << explicit_action;
  EXPECT_NE(explicit_action.find("is not proven"), std::string::npos) << explicit_action;
}

TEST(VideoAutoPolicyProbeTests, APrivateCompositorRouteDropsTheEncoderAnotherPlanProbedForTheCageProbe) {
  // labwc has to be started to probe at all, which the deferred cage probe does, so a refresh never
  // probes a cage route against the desktop. It used to keep the encoder another plan had probed: the
  // host went on advertising that encoder's codecs to Private Stream clients, and the deferred cage
  // probe, which runs only while no encoder is selected, never ran.
  const auto_probe_guard_t guard;
  ASSERT_EQ(probe_auto(amd_gamescope_stream).selection.selected_encoder, "vulkan");
  config::video.linux_display.stream_mode = "headless_stream";
  config::video.linux_display.private_runtime = "labwc";
  config::video.linux_display.use_cage_compositor = true;
  config::video.capture.clear();

  // While a stream or an app runs, its encoder stays.
  const auto running = refresh_amd_auto_plan(true);
  EXPECT_EQ(running.result, video::auto_plan_refresh_e::deferred);
  EXPECT_EQ(running.selection.selected_encoder, "vulkan");

  const auto cage = refresh_amd_auto_plan();
  EXPECT_EQ(cage.result, video::auto_plan_refresh_e::left_to_cage_probe);
  EXPECT_TRUE(cage.tried.empty());
  EXPECT_TRUE(video::active_encoder_name().empty());
  EXPECT_EQ(video::active_hevc_mode, config::video.hevc_mode);
  EXPECT_EQ(video::active_av1_mode, config::video.av1_mode);

  // An encoder the cage route probed itself is its own, and stays.
  ASSERT_EQ(probe_auto({"AMD Private Stream on labwc", "amdgpu", "headless_stream", "labwc", true}).selection.selected_encoder, "vulkan");
  const auto own = refresh_amd_auto_plan();
  EXPECT_EQ(own.result, video::auto_plan_refresh_e::current);
  EXPECT_EQ(own.selection.selected_encoder, "vulkan");
}

TEST(VideoAutoPolicyProbeTests, APrivateStreamHostAdvertisesNoDesktopCodecsAfterSteamGameModeHandsItBack) {
  // Steam Game Mode's hold swaps Private Stream for Mirror Desktop, and the refresh probes VA-API for
  // it, which offers AV1. When Game Mode handed Private Stream back, VA-API stayed selected, so the
  // deferred cage probe never ran and serverinfo kept VA-API's AV1 until the next Private Stream
  // launch probed Vulkan Video, which has none, and refused an AV1 client at ANNOUNCE. Before the
  // refresh nothing probed during the hold, and the labwc codecs stayed.
  const auto_probe_guard_t guard;
  const game_mode_hold_guard_t hold;
  using stream_display_policy::game_mode_reconcile_e;

  const auto launch = probe_auto({"AMD Private Stream on labwc", "amdgpu", "headless_stream", "labwc", true});
  ASSERT_EQ(launch.selection.policy, "amd_private_vulkan_live_probe");
  ASSERT_EQ(launch.advertised.av1_mode, 1);

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  const auto in_game_mode = refresh_amd_auto_plan();
  EXPECT_EQ(in_game_mode.result, video::auto_plan_refresh_e::reprobed);
  EXPECT_EQ(in_game_mode.selection.selected_encoder, "vaapi");
  EXPECT_EQ(in_game_mode.advertised.av1_mode, 3);

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  ASSERT_TRUE(config::video.linux_display.use_cage_compositor);
  const auto after_game_mode = refresh_amd_auto_plan();
  EXPECT_EQ(after_game_mode.result, video::auto_plan_refresh_e::left_to_cage_probe);
  EXPECT_TRUE(after_game_mode.tried.empty());
  EXPECT_TRUE(video::active_encoder_name().empty());
  EXPECT_EQ(video::active_av1_mode, config::video.av1_mode) << "VA-API's AV1 is not advertised to Private Stream";
}
#endif
#endif

#ifdef __linux__
TEST(VideoProbeDriverTests, RetainsAndVerifiesActualLoadedFileMappings) {
  platf::encoder_probe_identity::driver_proof_t proof;
  ASSERT_TRUE(proof.include_live_objects());
  const auto first = proof.current_key();
  ASSERT_TRUE(first);
  EXPECT_TRUE(proof.include_live_objects());
  EXPECT_EQ(first, proof.current_key());
}
#endif

#if defined(__linux__) && defined(POLARIS_PROBE_PROVIDER_FIXTURE)
namespace {
  class ProbeProviderFixture: public testing::Test {
  protected:
    void SetUp() override {
      char pattern[] = "/tmp/polaris-probe-provider-XXXXXX";
      const auto created = mkdtemp(pattern);
      ASSERT_NE(created, nullptr);
      directory = created;
      path = directory / "provider.so";
      std::filesystem::copy_file(POLARIS_PROBE_PROVIDER_FIXTURE, path);
    }
    void TearDown() override {
      if (handle) dlclose(handle);
      if (!directory.empty()) {
        std::filesystem::remove(path);
        std::filesystem::remove(directory / "replacement.so");
        std::filesystem::remove(directory);
      }
    }
    void load() {
      handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
      ASSERT_NE(handle, nullptr) << dlerror();
    }
    void unload() {
      ASSERT_NE(handle, nullptr);
      ASSERT_EQ(dlclose(handle), 0);
      handle = nullptr;
    }
    std::filesystem::path directory, path;
    void *handle = nullptr;
  };
}

TEST_F(ProbeProviderFixture, RetentionSurvivesCallerCloseAndReleasesAfterProof) {
  load();
  {
    platf::encoder_probe_identity::driver_proof_t proof;
    ASSERT_TRUE(proof.include_live_objects());
    const auto identity = proof.current_key();
    ASSERT_TRUE(identity);
    unload();
    EXPECT_EQ(proof.current_key(), identity);
    auto retained = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    ASSERT_NE(retained, nullptr);
    const auto entry = reinterpret_cast<int (*)()>(dlsym(retained, "polaris_probe_provider_fixture"));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry(), 42);
    EXPECT_EQ(dlclose(retained), 0);
    platf::encoder_probe_identity::driver_proof_t fresh;
    ASSERT_TRUE(fresh.include_live_objects());
    EXPECT_NE(fresh.current_key(), identity) << "new proofs never inherit old runtime authority";
  }
  auto released = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  EXPECT_EQ(released, nullptr);
  if (released) dlclose(released);
}

TEST_F(ProbeProviderFixture, PathReplacementInvalidatesRetainedOldMapping) {
  load();
  platf::encoder_probe_identity::driver_proof_t proof;
  ASSERT_TRUE(proof.include_live_objects());
  ASSERT_TRUE(proof.current_key());
  const auto replacement = directory / "replacement.so";
  std::filesystem::copy_file(POLARIS_PROBE_PROVIDER_FIXTURE, replacement);
  std::filesystem::rename(replacement, path);
  EXPECT_FALSE(proof.current_key());
  EXPECT_FALSE(proof.include_live_objects());
}

TEST_F(ProbeProviderFixture, InPlaceFileMetadataChangeInvalidatesProof) {
  load();
  platf::encoder_probe_identity::driver_proof_t proof;
  ASSERT_TRUE(proof.include_live_objects());
  ASSERT_TRUE(proof.current_key());
  // Do not corrupt a mapped ELF. An explicit timestamp change exercises the
  // same invalidation signal as an in-place driver update.
  std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(1));
  EXPECT_FALSE(proof.current_key());
}

TEST_F(ProbeProviderFixture, ConcurrentLoaderChangeCannotBeAbsorbedAtCollectionEnd) {
  platf::encoder_probe_identity::driver_proof_t proof;
  EXPECT_FALSE(proof.include_live_objects([&] {
    std::thread loader([&] { handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL); });
    loader.join();
    ASSERT_NE(handle, nullptr);
  }));
  EXPECT_FALSE(proof.current_key());
}

TEST_F(ProbeProviderFixture, LoaderChangeAfterCollectionInvalidatesKey) {
  platf::encoder_probe_identity::driver_proof_t proof;
  ASSERT_TRUE(proof.include_live_objects());
  ASSERT_TRUE(proof.current_key());
  load();
  EXPECT_FALSE(proof.current_key());
  unload();
  EXPECT_FALSE(proof.current_key()) << "load then unload still retires an earlier proof";
}

TEST(VideoProbeDriverTests, EveryMappedSegmentMustKeepItsDeviceInodeAndFileOffset) {
  using namespace platf::encoder_probe_identity;
  const mapped_file_t identity {0x1000, 0x3000, 0, 55, 8, 2};
  const std::vector<load_segment_t> segments {{0x1000, 0x2800, 0}};
  const std::vector<mapped_file_t> original {{0x1000, 0x2000, 0, 55, 8, 2},
                                           {0x2000, 0x3000, 0x1000, 55, 8, 2}};
  ASSERT_TRUE(mapped_to_file(segments, identity, original));
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto changed = original;
    switch (mutation) {
      case 0: ++changed[1].device_major; break;
      case 1: ++changed[1].device_minor; break;
      case 2: ++changed[1].inode; break;
      case 3: ++changed[1].offset; break;
      case 4: ++changed[1].start; break;
    }
    EXPECT_FALSE(mapped_to_file(segments, identity, changed));
  }
}

TEST_F(ProbeProviderFixture, ProviderSearchPrecedenceAndOverridesRetireReuse) {
  const std::vector<std::string> variables {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CONFIG_DIRS",
    "XDG_DATA_DIRS", "LD_LIBRARY_PATH", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS",
    "__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES", "__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS"};
  std::vector<std::optional<std::string>> previous;
  for (const auto &name : variables) {
    const auto value = getenv(name.c_str());
    previous.emplace_back(value ? std::make_optional<std::string>(value) : std::nullopt);
    unsetenv(name.c_str());
  }
  auto restore = util::fail_guard([&] {
    for (size_t i = 0; i < variables.size(); ++i) {
      if (previous[i]) setenv(variables[i].c_str(), previous[i]->c_str(), 1);
      else unsetenv(variables[i].c_str());
    }
  });
  setenv("HOME", directory.c_str(), 1);
  const auto forward = (directory / "first").string() + ":" + (directory / "second").string();
  const auto reverse = (directory / "second").string() + ":" + (directory / "first").string();
  setenv("XDG_CONFIG_DIRS", forward.c_str(), 1);
  const auto first = platf::encoder_probe_identity::provider_selection_key();
  ASSERT_TRUE(first);
  EXPECT_NE(first->find("/etc/xdg/vulkan/icd.d"), std::string::npos);
  setenv("XDG_CONFIG_DIRS", reverse.c_str(), 1);
  const auto second = platf::encoder_probe_identity::provider_selection_key();
  ASSERT_TRUE(second);
  EXPECT_NE(first, second);
  const auto external = directory / "first/egl/egl_external_platform.d";
  auto remove_external = util::fail_guard([&] {
    std::error_code ec;
    std::filesystem::remove_all(directory / "first", ec);
  });
  std::filesystem::create_directories(external);
  const auto manifest = external / "15_gbm.json";
  { std::ofstream output(manifest); output << R"({"library_path":"provider-one.so"})"; }
  const auto external_before = platf::encoder_probe_identity::provider_selection_key();
  ASSERT_TRUE(external_before);
  EXPECT_NE(external_before, second);
  { std::ofstream output(manifest); output << R"({"library_path":"provider-two.so"})"; }
  EXPECT_NE(platf::encoder_probe_identity::provider_selection_key(), external_before);
  for (const auto variable : {"LD_LIBRARY_PATH", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS",
         "__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES", "__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS"}) {
    setenv(variable, "/unobserved/provider", 1);
    EXPECT_FALSE(platf::encoder_probe_identity::provider_selection_key()) << variable;
    unsetenv(variable);
  }
  for (const auto variable : {"__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES", "__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS"}) {
    setenv(variable, "", 1);
    EXPECT_FALSE(platf::encoder_probe_identity::provider_selection_key()) << variable;
    unsetenv(variable);
  }
}
#endif

#if defined(__linux__) && defined(POLARIS_PROBE_PROVIDER_FIXTURE)
TEST_F(ProbeProviderFixture, NonregularProviderManifestCannotBlockTheEncoderWriter) {
  const auto manifest = directory / "replacement.so";
  ASSERT_EQ(mkfifo(manifest.c_str(), 0600), 0);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(platf::encoder_probe_identity::bounded_file(manifest));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}
#endif
TEST(VideoRateTests, PreservesExplicitMillihertzAndNormalizesOnlyDisplayHints) {
  const std::vector<std::tuple<int, int, AVRational>> cases {
    {60, 0, {60, 1}}, {60, 5994, {60000, 1001}}, {120, 11988, {120000, 1001}},
    {24, 2397, {24000, 1001}}, {24, 2398, {24000, 1001}}, {95, 9498, {4749, 50}},
    {59940, 5994, {2997, 50}}, {23976, 2398, {2997, 125}}, {60000, 11988, {60, 1}},
    {240, 6000, {240, 1}}, {4000, 6000, {4000, 1}}, {4001, 6000, {4001, 1000}}
  };
  for (const auto &[wire, hint, expected] : cases) {
    const auto actual = video::rate::from_wire(wire, hint);
    EXPECT_EQ(av_cmp_q(actual, expected), 0) << wire << '/' << hint;
  }
}

TEST(VideoRateTests, DisplayRateCannotRaiseOrReplaceTheRequestedStreamRate) {
  for (const auto hint : {0, -1, 12000, 11988, 3000, 6050, 5950, INT_MAX}) {
    EXPECT_EQ(av_cmp_q(video::rate::from_wire(60, hint), AVRational {60, 1}), 0) << hint;
  }
  EXPECT_FALSE(video::rate::valid(video::rate::from_wire(0, 6000)));
  EXPECT_FALSE(video::rate::valid(video::rate::from_wire(-60, 6000)));
  EXPECT_EQ(video::rate::interval({0, 1}), std::chrono::nanoseconds::zero());
  EXPECT_EQ(video::rate::interval({60, 0}), std::chrono::nanoseconds::zero());
  EXPECT_TRUE(video::rate::valid(video::rate::from_wire(INT_MAX, INT_MAX)));
  EXPECT_TRUE(video::rate::valid(video::rate::from_hundredths(INT_MAX)));
}

TEST(VideoRateTests, CaptureAndEncodingLimiterKeepSeparateWarpAndLaunchRates) {
  video::config_t config {};
  config.framerate = 240;  // Warp frame budget stays 240 FPS.
  config.encodingFramerate = 59940;  // Original launch millihertz contract.
  config.stream_rate = video::rate::from_wire(240, 5994);
  config.encode_rate = video::rate::from_millihertz(59940);
  EXPECT_EQ(video::capture_frame_interval(config), std::chrono::nanoseconds(4166666));
  EXPECT_EQ(video::encoding_frame_interval(config), std::chrono::nanoseconds(16683350));
  EXPECT_EQ(config.framerate, 240);
  EXPECT_EQ(config.encodingFramerate, 59940);
  config.stream_rate = video::rate::from_wire(60, 5994);
  config.encode_rate = config.stream_rate;  // Limiter disabled follows stream.
  EXPECT_EQ(video::capture_frame_interval(config), std::chrono::nanoseconds(16683333));
  EXPECT_EQ(video::encoding_frame_interval(config), video::capture_frame_interval(config));
}

#ifdef __linux__
TEST(VideoDisplaySelectionTests, KmsConnectorAliasesAreUniqueAndLegacyIndicesKeepTheirMapping) {
  const std::vector<std::string> displays {"kms:pci-0000:01:00.0/DP-1", "kms:pci-0000:03:00.0/DP-1", "kms:pci-0000:01:00.0/HDMI-A-1"};
  EXPECT_EQ(video::find_display_index_for_tests(displays, "0"), 0);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "1"), 1);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "2"), 2);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "3"), std::nullopt);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "DP-1"), std::nullopt);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "HDMI-A-1"), 2);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "pci-0000:03:00.0/DP-1"), 1);
  EXPECT_EQ(video::find_display_index_for_tests(displays, "kms:pci-0000:03:00.0/DP-1"), 1);
}
#endif

TEST(VideoDisplaySelectionTests, ActualRefreshWrapperDoesNotTurnMissingOrAmbiguousNamesIntoDisplayZero) {
  const std::vector<std::string> before {"kms:pci-0000:01:00.0/DP-1", "kms:pci-0000:03:00.0/DP-1"};
  EXPECT_EQ(video::refresh_display_selection_for_tests({}, -1, "missing-output", before), -1);
  EXPECT_EQ(video::refresh_display_selection_for_tests(before, 0, "", {before[1]}), -1);
  EXPECT_EQ(video::refresh_display_selection_for_tests({}, -1, "1", before), 1);
  EXPECT_EQ(video::refresh_display_selection_for_tests({}, -1, "", before), 0);
#ifdef __linux__
  EXPECT_EQ(video::refresh_display_selection_for_tests({}, -1, "DP-1", before), -1);
  EXPECT_EQ(video::refresh_display_selection_for_tests(before, 0, "", {before[1], before[0]}), 1);
#endif
}

TEST(VideoVulkanRcModeOptionTests, AutoMapsToNamedConstant) {
  // FFmpeg's Vulkan auto sentinel is FF_VK_RC_MODE_AUTO (0xFFFFFFFF), which does not fit in an int option;
  // a raw zero would select the driver's default rate control instead.
  EXPECT_EQ(video::vulkan_rc_mode_option(0), "auto");
}

TEST(VideoVulkanRcModeOptionTests, ExplicitModesPassThroughAsDriverFlags) {
  EXPECT_EQ(video::vulkan_rc_mode_option(1), "1");
  EXPECT_EQ(video::vulkan_rc_mode_option(2), "2");
  EXPECT_EQ(video::vulkan_rc_mode_option(4), "4");
}

TEST(VideoVulkanQualityClampTests, UnknownDriverCountPassesThroughUnchanged) {
  // -1 means no live probe has reported a count yet; FFmpeg validates the level at session open.
  EXPECT_EQ(video::vulkan_quality_clamp(0, -1), 0);
  EXPECT_EQ(video::vulkan_quality_clamp(3, -1), 3);
}

TEST(VideoVulkanQualityClampTests, ClampsToDriverReportedMaximum) {
  // Valid levels run 0..count-1; FFmpeg's own guard lets level == count through.
  EXPECT_EQ(video::vulkan_quality_clamp(2, 4), 2);
  EXPECT_EQ(video::vulkan_quality_clamp(3, 4), 3);
  EXPECT_EQ(video::vulkan_quality_clamp(4, 4), 3);
  EXPECT_EQ(video::vulkan_quality_clamp(9, 1), 0);
}

TEST(VideoVulkanQualityClampTests, AdvertisedMaximumIgnoresAv1WhileTheEncoderKeepsItOff) {
  // Counts are indexed H.264, HEVC, AV1. A lower AV1 count must not cap H.264 and HEVC
  // on the Vulkan encoder, which keeps AV1 fail-closed.
  EXPECT_EQ(video::vulkan_quality_max({4, 4, 2}, false), 3);
  EXPECT_EQ(video::vulkan_quality_max({4, 4, 2}, true), 1);
  EXPECT_EQ(video::vulkan_quality_max({4, 3, 4}, false), 2);
  EXPECT_EQ(video::vulkan_quality_max({-1, 4, -1}, false), 3);
  EXPECT_EQ(video::vulkan_quality_max({-1, -1, 4}, false), -1);
  EXPECT_EQ(video::vulkan_quality_max({-1, -1, -1}, true), -1);
}

TEST(VideoVulkanQualityClampTests, NegativeConfiguredValueFloorsAtZero) {
  EXPECT_EQ(video::vulkan_quality_clamp(-1, 4), 0);
  EXPECT_EQ(video::vulkan_quality_clamp(-5, -1), 0);
}

namespace {

  /// What a client that has read the SDP and the codec mode bits would send for SDR.
  video::config_t pyrowave_request() {
    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = 60;
    config.bitrate = 100000;
    config.videoFormat = video::VIDEO_FORMAT_PYROWAVE;
    // Full range Rec. 709: bit 0 for full, colourspace 1 above it.
    config.encoderCscMode = 3;
    config.dynamicRange = 0;
    config.chromaSamplingType = 0;
    return config;
  }

}  // namespace

TEST(PyroWaveAnnounceTests, AServableRequestIsNotRefused) {
  const auto config = pyrowave_request();
  EXPECT_FALSE(video::pyrowave_announce_refusal(config, true, true).has_value());
  EXPECT_FALSE(video::pyrowave_announce_refusal(config, true, false).has_value())
    << "an SDR request does not need the host to be able to do HDR";
}

TEST(PyroWaveAnnounceTests, AHostWithoutTheCodecRefusesEverything) {
  const auto refusal = video::pyrowave_announce_refusal(pyrowave_request(), false, false);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_NE(refusal->find("cannot run"), std::string::npos) << *refusal;
}

TEST(PyroWaveAnnounceTests, HdrNeedsTheHostToHaveIt) {
  auto config = pyrowave_request();
  config.dynamicRange = 1;
  config.encoderCscMode = 5;  // full range BT.2020

  EXPECT_FALSE(video::pyrowave_announce_refusal(config, true, true).has_value());

  // The bit in ServerCodecModeSupport is the only warning a client gets, and it reads that before it
  // launches. A host that can encode but cannot carry HDR has to say no rather than send Rec. 709
  // under BT.2020 PQ metadata, which is the dark oversaturated picture people report as broken HDR.
  const auto refusal = video::pyrowave_announce_refusal(config, true, false);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_NE(refusal->find("HDR"), std::string::npos) << *refusal;
}

TEST(PyroWaveAnnounceTests, AnHdrRequestMayNameEither709Or2020) {
  auto config = pyrowave_request();
  config.dynamicRange = 1;

  // Polaris derives BT.2020 PQ from the dynamic range rather than from this field, so both of these
  // produce the same stream and refusing one would be refusing over a field nothing reads.
  config.encoderCscMode = 3;  // full range, colourspace says 709
  EXPECT_FALSE(video::pyrowave_announce_refusal(config, true, true).has_value());
  config.encoderCscMode = 5;  // full range, colourspace says 2020
  EXPECT_FALSE(video::pyrowave_announce_refusal(config, true, true).has_value());

  // 601 is not one of the two.
  config.encoderCscMode = 1;
  EXPECT_TRUE(video::pyrowave_announce_refusal(config, true, true).has_value());
}

TEST(PyroWaveAnnounceTests, LimitedRangeIsRefusedInBothRanges) {
  // The codec's colour conversion is full range and has no setting for it, so a client asking for
  // limited range would decode every frame with the wrong maths: raised blacks, flattened whites, and
  // nothing in any log to say why. This is the check that a stream cannot silently be that.
  for (const int dynamic_range : {0, 1}) {
    auto config = pyrowave_request();
    config.dynamicRange = dynamic_range;
    config.encoderCscMode = dynamic_range == 1 ? 4 : 2;  // colourspace right, range bit clear

    const auto refusal = video::pyrowave_announce_refusal(config, true, true);
    ASSERT_TRUE(refusal.has_value()) << "limited range accepted at dynamic range " << dynamic_range;
    EXPECT_NE(refusal->find("full range"), std::string::npos) << *refusal;
  }
}

TEST(PyroWaveAnnounceTests, SdrMustBeRec709) {
  auto config = pyrowave_request();
  config.encoderCscMode = 5;  // full range, but BT.2020 on an eight bit stream

  // Without the dynamic range to derive BT.2020 PQ from, this field is the only thing that says which
  // matrix the client will invert, and the encoder only makes one of them.
  const auto refusal = video::pyrowave_announce_refusal(config, true, true);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_NE(refusal->find("Rec. 709"), std::string::npos) << *refusal;
}

TEST(PyroWaveAnnounceTests, AnOddExtentIsRefused) {
  for (const auto [width, height] : {std::pair {1921, 1080}, std::pair {1920, 1081},
                                     std::pair {0, 1080}, std::pair {1920, -2}}) {
    auto config = pyrowave_request();
    config.width = width;
    config.height = height;

    const auto refusal = video::pyrowave_announce_refusal(config, true, true);
    ASSERT_TRUE(refusal.has_value()) << width << 'x' << height << " was accepted";
    EXPECT_NE(refusal->find("even stream size"), std::string::npos) << *refusal;
  }
}

namespace {
  std::string video_source_for_contract(const char *relative) {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / relative;
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
}  // namespace

/**
 * A session that cannot read what capture produces has to end the stream, not fail a frame.
 *
 * Any non-zero answer from convert() fails one frame, and the capture thread answers a failed frame
 * by building the session again. For a frame that arrived wrong that is right. For a session whose
 * codec reads ten bit while capture produces eight, the new session is identical and refuses the
 * identical frame: a thousand sessions in two minutes, each logging the same sentence, with the
 * client seeing a stream that never starts and no reason anywhere it can show.
 */
TEST(PyroWaveAnnounceTests, AFormatThisSessionCanNeverReadEndsTheStream) {
  const auto video = video_source_for_contract("src/video.cpp");
  ASSERT_FALSE(video.empty());
  EXPECT_NE(video.find("return convert_session_is_over;"), std::string::npos)
    << "the unreadable format refuses one frame at a time, so the host rebuilds the session forever";

  EXPECT_NE(video.find("if (converted == convert_session_is_over)"), std::string::npos)
    << "the parallel capture thread rebuilds a session that already said it cannot continue";
  // The synchronous path already ends the stream after the route handler, and no session on this
  // codec runs there anyway, so it is left exactly as it was.
}

/**
 * Both places the encode loop ends a stream its encoder can never serve tell the client why.
 *
 * Raising the shutdown event alone ended the stream with a bare disconnect, which a Moonlight client
 * answers by reconnecting into the same refusal. The helper raises the reason first, and the stream
 * control tests hold what the control thread sends for it.
 */
TEST(PyroWaveAnnounceTests, TheLoopEndsAStreamItsEncoderCannotServeWithTheReason) {
  const auto video = video_source_for_contract("src/video.cpp");
  ASSERT_FALSE(video.empty());
  // The loop's convert branch, and the session that could not be built for a reason that will still
  // hold on the next attempt.
  for (const auto *opening : {"if (converted == convert_session_is_over) {",
                              "// Returning alone leaves the host to build this session again"}) {
    const auto branch = video.find(opening);
    ASSERT_NE(branch, std::string::npos) << opening;
    const auto told = video.find("end_stream_encoder_cannot_serve(mail);", branch);
    const auto closed = video.find('}', branch);
    ASSERT_NE(told, std::string::npos) << opening;
    EXPECT_LT(told, closed) << opening << " ends the stream without saying why";
  }
}

TEST(PyroWaveAnnounceTests, TheEndOfStreamAnswerIsNotSomethingAFrameCanMean) {
  EXPECT_LT(video::convert_session_is_over, 0);
  EXPECT_NE(video::convert_session_is_over, -1);
}

namespace {
  /// A device that answers every frame the same way, which is all the converter in front of it reads.
  struct answering_device_t: platf::avcodec_encode_device_t {
    explicit answering_device_t(int answer):
        answer {answer} {
    }

    int convert(platf::img_t &) override {
      return answer;
    }

    int answer;
  };

  int converted_by(int device_answer) {
    auto img = std::make_shared<platf::img_t>();
    static std::vector<std::uint8_t> pixels(64 * 4 * 4);
    img->data = pixels.data();
    img->width = 64;
    img->height = 4;
    img->pixel_pitch = 4;
    img->row_pitch = 64 * 4;
    video::frame_t frame {img};
    return video::convert_with_encode_device_for_tests(std::make_unique<answering_device_t>(device_answer), frame);
  }
}  // namespace

/**
 * A device that can never read what capture hands it ends the stream once, through the converter.
 *
 * The converter an avcodec session puts in front of its device used to answer every failure with
 * -1, which the capture thread reads as one bad frame and answers by building the session again. The
 * Vulkan Video system memory upload meeting a DMA-BUF frame is a device that cannot read anything
 * this capture will produce, and a session built again meets the same frame: a new session for every
 * frame, with no picture ever sent.
 */
TEST(VideoFrameConverterTests, ADeviceThatCanNeverReadTheCaptureEndsTheStream) {
  EXPECT_EQ(converted_by(platf::convert_capture_unreadable), video::convert_session_is_over)
    << "the converter turned the device's answer into one failed frame, so the host builds the session again";
  EXPECT_EQ(converted_by(-1), -1) << "a frame that went wrong is still one frame";
  EXPECT_EQ(converted_by(0), 0);
}

TEST(PyroWaveAnnounceTests, ADynamicRangeThisCodecDoesNotKnowIsRefused) {
  auto config = pyrowave_request();
  config.dynamicRange = 2;

  const auto refusal = video::pyrowave_announce_refusal(config, true, true);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_NE(refusal->find("two dynamic ranges"), std::string::npos) << *refusal;
}

TEST(VideoMain10Authority, ReadsKwinAsThePortalItOpens) {
#ifdef __linux__
  // The portal probes an NV12 dummy, so on a Gamescope Stream host that captures through it a
  // failed 10-bit probe keeps the configured mode. A host set to kwin captures through the same
  // portal, and the literal comparison let that probe take Main10 away from it.
  EXPECT_FALSE(video::main10_probe_is_authoritative("portal", "gamescope_stream"));
  EXPECT_FALSE(video::main10_probe_is_authoritative("kwin", "gamescope_stream"));
  EXPECT_TRUE(video::main10_probe_is_authoritative("kms", "gamescope_stream"));
  EXPECT_TRUE(video::main10_probe_is_authoritative("drm", "gamescope_stream"));
  EXPECT_TRUE(video::main10_probe_is_authoritative("portal", "desktop_display"));
  EXPECT_TRUE(video::main10_probe_is_authoritative("kwin", "desktop_display"));
#else
  EXPECT_TRUE(video::main10_probe_is_authoritative("portal", "gamescope_stream"));
#endif
}

#if defined(POLARIS_TESTS) && defined(__linux__)
namespace {
  video::config_t capture_session_config(std::uint64_t session_generation) {
    video::config_t config {};
    config.session_generation = session_generation;
    config.capture_generation.capture_backend = "kwin";
    config.capture_request.preference = "kwin";
    return config;
  }

  nlohmann::json capture_of_client(std::size_t index) {
    return nlohmann::json::parse(stream_stats::get_current().to_json())["clients"][index].value("capture", nlohmann::json {});
  }
}  // namespace

TEST(VideoCaptureBackendPublicationTests, EachConsumingSessionPublishesUntilItsOwnGenerationLands) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  const platf::capture_route_t route {"portal", "portal_kwin_node", ""};
  const auto owner = capture_session_config(391);
  bool owner_published = false;
  // The video thread opened the display before the session registered its stats entry.
  EXPECT_FALSE(video::publish_capture_backend_for_tests(owner, route, owner_published));
  stream_stats::add_client("10.0.0.5", "Owner", 391);
  EXPECT_TRUE(video::publish_capture_backend_for_tests(owner, route, owner_published));

  // A second viewer joins the display the owner opened, and publishes it for itself.
  const auto viewer = capture_session_config(392);
  bool viewer_published = false;
  stream_stats::add_client("10.0.0.6", "Viewer", 392);
  EXPECT_TRUE(video::publish_capture_backend_for_tests(viewer, route, viewer_published));
  for (std::size_t client = 0; client < 2; ++client) {
    const auto capture = capture_of_client(client);
    EXPECT_EQ(capture.value("preference", ""), "kwin") << "polaris.conf as written";
    EXPECT_EQ(capture.value("requested", ""), "portal") << "read the way dispatch reads it";
    EXPECT_EQ(capture.value("opened", ""), "portal");
    EXPECT_EQ(capture.value("route", ""), "portal_kwin_node");
  }

  // No generation and nothing opened are nothing to publish.
  bool legacy_published = false;
  EXPECT_FALSE(video::publish_capture_backend_for_tests(capture_session_config(0), route, legacy_published));
  bool unopened_published = false;
  EXPECT_FALSE(video::publish_capture_backend_for_tests(capture_session_config(391), {}, unopened_published));
}

TEST(VideoCaptureBackendPublicationTests, AProbeNeverPublishesWhatItsDisplayOpened) {
  const auto old_config = config::video;
  // The validate hook sets the global NVENC capabilities, and ctest runs this binary as one process.
  const auto old_h264 = video::nvenc.h264.capabilities;
  const auto old_hevc = video::nvenc.hevc.capabilities;
  const auto old_av1 = video::nvenc.av1.capabilities;
  auto restore = util::fail_guard([&] {
    video::reset_encoder_probe_state();
    config::video = old_config;
    video::nvenc.h264.capabilities = old_h264;
    video::nvenc.hevc.capabilities = old_hevc;
    video::nvenc.av1.capabilities = old_av1;
    stream_stats::update_stream_active(false);
  });
  stream_stats::update_stream_active(false);
  video::reset_encoder_probe_state();
  config::video.encoder = "nvenc";
  config::video.linux_display.use_cage_compositor = true;
  stream_stats::add_client("10.0.0.5", "Client", 393);
  const platf::capture_route_t route {"wlr", "wlr", ""};
  const auto session = capture_session_config(393);
  bool published = false;
  std::optional<bool> published_in_probe;
  auto validate = [&](video::encoder_t &encoder, bool) {
    encoder.h264.capabilities.set();
    if (!published_in_probe) {
      published_in_probe = video::publish_capture_backend_for_tests(session, route, published);
    }
    return true;
  };
  EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(complete_probe_identity(), validate), 0);
  ASSERT_TRUE(published_in_probe.has_value()) << "the probe validated nothing";
  EXPECT_FALSE(*published_in_probe);
  EXPECT_TRUE(capture_of_client(0).is_null());
  // The same session publishes from its own encode loop.
  EXPECT_TRUE(video::publish_capture_backend_for_tests(session, route, published));
  EXPECT_EQ(capture_of_client(0).value("opened", ""), "wlr");
}

TEST(VideoCaptureBackendPublicationTests, AcceptedFramesCarryTheirSourceFormatAndALandingWritesTheNextOneAgain) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  const platf::capture_route_t route {"portal", "portal_screencast", ""};
  auto session = capture_session_config(394);
  session.width = 1280;
  session.height = 720;
  std::array<std::uint8_t, 4> pixels {};
  video::frame_t frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.cpu_data = pixels.data();
  // What the capture delivered, and what conversion made of it for the encoder.
  frame.source_metadata = {platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu, platf::frame_format_e::p010, {}};
  frame.metadata = {platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu, platf::frame_format_e::nv12, {}};
  std::optional<stream_stats::capture_source_t> reported;
  bool published = false;

  // The video thread opened the display before the session registered, and the session's first
  // frame lands between the refused attempt and the retry.
  EXPECT_FALSE(video::publish_capture_backend_for_tests(session, route, published, reported));
  stream_stats::add_client("10.0.0.5", "Client", 394);
  video::record_capture_source_for_tests(session, frame, reported);
  ASSERT_TRUE(reported.has_value());
  EXPECT_TRUE(video::publish_capture_backend_for_tests(session, route, published, reported));
  EXPECT_EQ(capture_of_client(0).value("transport", ""), "unknown")
    << "a landing starts the frames over for the display it names";

  // The next accepted frame is written although it matches the last one, or the capture would
  // read unknown for as long as the frames stay the same.
  video::record_capture_source_for_tests(session, frame, reported);
  auto capture = capture_of_client(0);
  EXPECT_EQ(capture.value("transport", ""), "dmabuf");
  EXPECT_EQ(capture.value("residency", ""), "gpu");
  EXPECT_EQ(capture.value("format", ""), "p010") << "the source frame's format, not the converted frame's";

  // A change of format alone is written, through the record the loop keeps.
  frame.source_metadata.format = platf::frame_format_e::bgra8;
  video::record_capture_source_for_tests(session, frame, reported);
  EXPECT_EQ(capture_of_client(0).value("format", ""), "bgra8");
}

TEST(VideoPyroWaveRouteTests, ASessionWritesItsRouteWhenItMovesAndKeepsItOnlyOnceItLands) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  const auto route_of_client = [] {
    return nlohmann::json::parse(stream_stats::get_current().to_json())["clients"][0].value("pyrowave_route", "");
  };
  const auto session = capture_session_config(397);
  std::string reported;

  // The first frame can be encoded before the session registers its stats entry.
  video::record_pyrowave_route_for_tests(session, "gpu_upload", reported);
  EXPECT_EQ(reported, "") << "a write that did not land was kept, so the route is never written";
  stream_stats::add_client("10.0.0.5", "Client", 397);
  video::record_pyrowave_route_for_tests(session, "", reported);
  EXPECT_EQ(route_of_client(), "") << "unknown was written as a route";
  video::record_pyrowave_route_for_tests(session, "gpu_upload", reported);
  EXPECT_EQ(reported, "gpu_upload");
  EXPECT_EQ(route_of_client(), "gpu_upload");

  // A GPU path that falls back moves the route, and the move is written.
  video::record_pyrowave_route_for_tests(session, "cpu_convert", reported);
  EXPECT_EQ(route_of_client(), "cpu_convert");

  // Generation zero names no session, so nothing it encodes lands on a session's entry.
  std::string unowned;
  video::record_pyrowave_route_for_tests(capture_session_config(0), "zero_copy", unowned);
  EXPECT_EQ(unowned, "");
  EXPECT_EQ(route_of_client(), "cpu_convert");
}

/**
 * A display that opens starts the session's PyroWave route over, in the stats and in the loop.
 *
 * The encoder built for a new display has encoded nothing, so the route the one before it took is no
 * answer for it. The loop's record starts over with the landing too, or a route the session wrote
 * while its publication was still refused would be the one it never writes again.
 */
TEST(VideoPyroWaveRouteTests, ALandingStartsTheRouteOverForTheDisplayItNames) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  const auto route_of_client = [] {
    return nlohmann::json::parse(stream_stats::get_current().to_json())["clients"][0].value("pyrowave_route", "");
  };
  const auto session = capture_session_config(396);
  const platf::capture_route_t route {"portal", "portal_screencast", ""};
  std::optional<stream_stats::capture_source_t> reported_source;
  std::string reported_route;
  bool published = false;

  // The video thread opened the display before the session registered, and the session's first
  // frame lands between the refused attempt and the retry.
  EXPECT_FALSE(video::publish_capture_backend_for_tests(session, route, published, reported_source, reported_route));
  stream_stats::add_client("10.0.0.5", "Client", 396);
  video::record_pyrowave_route_for_tests(session, "zero_copy", reported_route);
  ASSERT_EQ(route_of_client(), "zero_copy");
  EXPECT_TRUE(video::publish_capture_backend_for_tests(session, route, published, reported_source, reported_route));
  EXPECT_EQ(route_of_client(), "") << "a landing kept the route the session wrote before it";
  EXPECT_EQ(reported_route, "") << "the loop kept a route the landing cleared";

  // The next frame's route is written although it matches the last one.
  video::record_pyrowave_route_for_tests(session, "zero_copy", reported_route);
  EXPECT_EQ(route_of_client(), "zero_copy") << "the route stays unknown for as long as it does not move";

  // Capture reinitializes. A fresh loop publishes the new display, and until that display's
  // encoder takes a frame the session has no route.
  bool reopened_published = false;
  std::optional<stream_stats::capture_source_t> reopened_source;
  std::string reopened_route;
  EXPECT_TRUE(video::publish_capture_backend_for_tests(session, {"portal", "portal_kwin_node", ""},
                                                       reopened_published, reopened_source, reopened_route));
  EXPECT_EQ(route_of_client(), "") << "a display that has encoded nothing reads the last display's route";
  video::record_pyrowave_route_for_tests(session, "gpu_upload", reopened_route);
  EXPECT_EQ(route_of_client(), "gpu_upload");
}

TEST(VideoPyroWaveRouteTests, TheEncodeLoopRecordsTheRouteOfEveryFrameItAccepts) {
  const auto video = video_source_for_contract("src/video.cpp");
  ASSERT_FALSE(video.empty());
  // Where the call sits. What it records is held below, by running it.
  const std::string accepted =
    "          record_capture_source(config, frame, reported_source);\n"
    "#ifdef POLARIS_BUILD_PYROWAVE\n"
    "          record_pyrowave_route(config, *session, reported_pyrowave_route);\n"
    "#endif\n";
  EXPECT_NE(video.find(accepted), std::string::npos)
    << "the parallel encode loop does not publish the PyroWave route beside the frame the session accepted";
}

/**
 * encode_time_ms counts PyroWave's time in convert(), and Live Tuning's encoder pressure does not.
 *
 * Live Tuning answers a slow encode by cutting bitrate. PyroWave spends that time on colour conversion
 * and a wavelet transform, which take as long at any bitrate, so handing it to the controller would
 * cut a 4K PyroWave stream on the CPU colour path toward its floor and give the encoder nothing back.
 */
TEST(VideoPyroWaveRouteTests, LiveTuningIsNotHandedEncodeTimeABitrateCutCannotShorten) {
  const auto video = video_source_for_contract("src/video.cpp");
  ASSERT_FALSE(video.empty());
  EXPECT_NE(video.find("const double bitrate_relievable_encode_ms = encode_duration - codec_time_in_convert;"),
            std::string::npos);
  const auto health = video.find("adaptive_bitrate::update_stream_health(");
  ASSERT_NE(health, std::string::npos);
  const auto call = video.substr(health, video.find(");", health) - health);
  EXPECT_NE(call.find("bitrate_relievable_encode_ms"), std::string::npos) << call;
  EXPECT_EQ(call.find("encode_duration"), std::string::npos) << call;
}

/**
 * The route the loop publishes is the one the session the host builds reports.
 *
 * The call the encode loop makes reads it from the codec session through the wrapper the host builds
 * for a PyroWave stream. If either answered unknown, every stream would say its route is not known
 * yet, forever, while the encoder and stats tests passed on routes they are handed.
 */
TEST(VideoPyroWaveRouteTests, TheLoopRecordsTheRouteOfTheSessionTheHostBuilds) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  const auto route_of_client = [](std::size_t index) {
    return nlohmann::json::parse(stream_stats::get_current().to_json())["clients"][index].value("pyrowave_route", "");
  };
  const auto session_for = [](std::uint64_t generation) {
    auto config = capture_session_config(generation);
    config.width = 640;
    config.height = 360;
    config.framerate = 60;
    config.bitrate = 20000;
    return config;
  };

  stream_stats::add_client("10.0.0.7", "Deck", 398);
  const auto on_the_gpu = video::pyrowave_route_of_a_host_frame_for_tests(session_for(398));
  if (!on_the_gpu) {
    GTEST_SKIP() << "no Vulkan device PyroWave can use";
  }
  EXPECT_EQ(*on_the_gpu, "gpu_upload") << "the loop did not record the route of a frame copied from host memory";
  EXPECT_EQ(route_of_client(0), "gpu_upload");

  stream_stats::add_client("10.0.0.8", "Tablet", 399);
  setenv("POLARIS_PYROWAVE_GPU_INPUT", "off", 1);
  const auto on_the_cpu = video::pyrowave_route_of_a_host_frame_for_tests(session_for(399));
  unsetenv("POLARIS_PYROWAVE_GPU_INPUT");
  ASSERT_TRUE(on_the_cpu);
  EXPECT_EQ(*on_the_cpu, "cpu_convert") << "the loop did not record the route of a frame converted on the CPU";
  EXPECT_EQ(route_of_client(1), "cpu_convert");
  EXPECT_EQ(route_of_client(0), "gpu_upload") << "one session's route landed on another's entry";
}

TEST(VideoCaptureBackendPublicationTests, AProbeNeverReachesTheLastSession) {
  const auto old_config = config::video;
  // The validate hook sets the global NVENC capabilities, and ctest runs this binary as one process.
  const auto old_h264 = video::nvenc.h264.capabilities;
  const auto old_hevc = video::nvenc.hevc.capabilities;
  const auto old_av1 = video::nvenc.av1.capabilities;
  auto restore = util::fail_guard([&] {
    video::reset_encoder_probe_state();
    config::video = old_config;
    video::nvenc.h264.capabilities = old_h264;
    video::nvenc.hevc.capabilities = old_hevc;
    video::nvenc.av1.capabilities = old_av1;
    stream_stats::update_stream_active(false);
  });
  const auto last_session = [] {
    const auto json = nlohmann::json::parse(stream_stats::get_current().to_json());
    return json.contains("last_session") ? json["last_session"] : nlohmann::json {};
  };
  stream_stats::update_stream_active(false);
  video::reset_encoder_probe_state();
  config::video.encoder = "nvenc";
  config::video.linux_display.use_cage_compositor = true;
  auto session = capture_session_config(396);
  session.width = 1280;
  session.height = 720;
  std::array<std::uint8_t, 4> pixels {};
  video::frame_t frame;
  frame.width = 1920;
  frame.height = 1080;
  frame.cpu_data = pixels.data();
  frame.source_metadata = {platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu, platf::frame_format_e::nv12, {}};
  std::optional<stream_stats::capture_source_t> reported;
  bool published = false;
  stream_stats::add_client("10.0.0.5", "Client", 396);
  ASSERT_TRUE(video::publish_capture_backend_for_tests(session, {"wlr", "wlr", ""}, published, reported));
  video::record_capture_source_for_tests(session, frame, reported);

  // A probe can run while the stream is still registered, and it opens a display of its own. It
  // tries to publish that display under the session's config. Had it landed, the session would
  // end with the probe's display in its last session.
  std::optional<bool> published_in_probe;
  auto validate = [&](video::encoder_t &encoder, bool) {
    encoder.h264.capabilities.set();
    if (!published_in_probe) {
      bool probe_published = false;
      std::optional<stream_stats::capture_source_t> probe_reported;
      published_in_probe = video::publish_capture_backend_for_tests(
        session, {"portal", "portal_screencast", ""}, probe_published, probe_reported
      );
    }
    return true;
  };
  EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(complete_probe_identity(), validate), 0);
  ASSERT_TRUE(published_in_probe.has_value()) << "the probe validated nothing";
  EXPECT_FALSE(*published_in_probe);

  stream_stats::remove_client("10.0.0.5", 396);
  const auto frozen = last_session();
  ASSERT_EQ(frozen.value("stream_instance_id", ""), stream_stats::stream_instance_id(396)) << frozen.dump();
  EXPECT_EQ(frozen["capture"].value("opened", ""), "wlr") << "the session's own display, not the probe's";
  EXPECT_EQ(frozen["capture"].value("route", ""), "wlr");
  EXPECT_EQ(frozen["capture"].value("transport", ""), "dmabuf") << frozen.dump();

  // A probe after the end finds no entry to reach, and only the next session's end replaces it.
  video::reset_encoder_probe_state();
  published_in_probe.reset();
  EXPECT_EQ(video::probe_encoders_with_hooks_for_tests(complete_probe_identity(), validate), 0);
  ASSERT_TRUE(published_in_probe.has_value()) << "the probe after the end validated nothing";
  EXPECT_FALSE(*published_in_probe);
  EXPECT_EQ(last_session(), frozen);
}
#endif

TEST(NvencVbvBufferTests, AnIncreaseAt500MbpsFitsTheEncodersField) {
  // One frame at the 500 Mbps a client may set by hand, 60 fps: 8333333 bits. nvenc_vbv_increase goes
  // to 400%, and from 258% the product overflowed an int before the division.
  constexpr std::int64_t int_max = std::numeric_limits<int>::max();
  constexpr std::int64_t uint32_max = std::numeric_limits<uint32_t>::max();
  const std::int64_t frame = 500000LL * 1000 / 60;
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(frame, 258, int_max), 29833332);
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(frame, 400, int_max), 41666665);
  // At 30 fps the NVENC SDK's uint32 field wrapped from 258% the same way.
  const std::int64_t slow = 500000LL * 1000 / 30;
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(slow, 258, uint32_max), 59666664);
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(slow, 400, uint32_max), 83333330);
  // No increase leaves the buffer alone, and a buffer the field cannot hold stops at the field.
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(frame, 0, int_max), frame);
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(int_max, 400, int_max), int_max);
  EXPECT_EQ(nvenc::grown_vbv_buffer_bits(uint32_max, 400, uint32_max), uint32_max);

  // Both encoders grow the buffer through it, not in the field's own type.
  const auto read = [](const char *relative) {
    std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / relative);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  };
  const auto video = read("src/video.cpp");
  ASSERT_FALSE(video.empty());
  EXPECT_NE(video.find("ctx->rc_buffer_size = static_cast<int>(nvenc::grown_vbv_buffer_bits("), std::string::npos);
  EXPECT_EQ(video.find("ctx->rc_buffer_size += ctx->rc_buffer_size *"), std::string::npos);
  const auto nvenc = read("src/nvenc/nvenc_base.cpp");
  ASSERT_FALSE(nvenc.empty());
  EXPECT_NE(nvenc.find("enc_config.rcParams.vbvBufferSize = static_cast<uint32_t>(grown_vbv_buffer_bits("), std::string::npos);
  EXPECT_EQ(nvenc.find("vbvBufferSize += enc_config.rcParams.vbvBufferSize *"), std::string::npos);
}
