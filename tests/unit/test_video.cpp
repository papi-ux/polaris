/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <src/video.h>

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

TEST(VideoCacheTests, ResetDisplayRetryDelayBackoffCapsAtTwoHundredMilliseconds) {
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(0), std::chrono::milliseconds(50));
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(1), std::chrono::milliseconds(100));
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(2), std::chrono::milliseconds(200));
  EXPECT_EQ(video::reset_display_retry_delay_for_tests(3), std::chrono::milliseconds(200));
}

TEST(VideoFrameConversionTests, InfersPackedBgr0InputStrideWhenRowPitchIsMissing) {
  EXPECT_EQ(video::software_frame_input_linesize_for_tests(0, 0, 0, 2560, AV_PIX_FMT_BGR0), 10240);
}

TEST(VideoFrameConversionTests, PrefersCaptureProvidedRowPitch) {
  EXPECT_EQ(video::software_frame_input_linesize_for_tests(8192, 4, 1920, 1920, AV_PIX_FMT_BGR0), 8192);
}

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
