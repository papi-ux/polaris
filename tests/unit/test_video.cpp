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
    "capture=;encoder=nvenc;adapter=;output=;cage=1;headless=1;auto_manage=0;displays=0",
    "nvenc",
    capability_state
  ));

  const auto cached = video::read_encoder_probe_cache_for_tests(
    cache_path,
    "580.142",
    "capture=;encoder=nvenc;adapter=;output=;cage=1;headless=1;auto_manage=0;displays=0"
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
#endif
