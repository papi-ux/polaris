/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <src/video.h>
#include <src/encoder_probe_reuse.h>
#include <thread>
#include <future>
#ifdef __linux__
#include <src/platform/linux/encoder_probe_driver_proof.h>
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
    "XDG_DATA_DIRS", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS"};
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
  for (const auto variable : {"VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS"}) {
    setenv(variable, "/unobserved/provider", 1);
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
