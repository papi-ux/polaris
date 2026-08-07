/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include "../tests_common.h"

#include <src/audio.h>
#include <src/config.h>

using namespace audio;

namespace {
  audio_ctx_t make_sink_context() {
    audio_ctx_t ctx {};
    ctx.sink.host = "alsa_output.host";
    ctx.sink.null = platf::sink_t::null_t {
      "sink-sunshine-stereo",
      "sink-sunshine-surround51",
      "sink-sunshine-surround71",
    };
    return ctx;
  }
}

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  std::bitset<3> result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {5, 2, 0x3, {0}, config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {5, 6, 0x3F, {0}, config_flags()}),
    std::make_tuple("SURROUND71", config_t {5, 8, 0x63F, {0}, config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)})
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::thread capture([&] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}

namespace {
  struct fake_audio_control_t: platf::audio_control_t {
    int set_sink(const std::string &) override {
      return 0;
    }

    std::unique_ptr<platf::mic_t> microphone(const std::uint8_t *, int, std::uint32_t, std::uint32_t, const std::string &, bool) override {
      return nullptr;
    }

    bool is_sink_available(const std::string &) override {
      return true;
    }

    std::optional<platf::sink_t> sink_info() override {
      return std::nullopt;
    }
  };
}  // namespace

TEST(AudioSinkSelectionTest, SelectsVirtualSinkWhenHostAudioIsDisabled) {
  auto old_sink = config::audio.sink;
  config::audio.sink.clear();
  auto ctx = make_sink_context();

  const auto sink = audio::select_sink_name(ctx, 2, false);

  EXPECT_EQ(sink, "sink-sunshine-stereo");
  EXPECT_TRUE(audio::sink_is_virtual(ctx, sink));
#ifdef __linux__
  EXPECT_TRUE(audio::should_claim_default_sink(ctx, sink, false));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
#else
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
#endif

  config::audio.sink = old_sink;
}

TEST(AudioSinkSelectionTest, SelectsHostSinkWhenHostAudioIsEnabled) {
  auto old_sink = config::audio.sink;
  config::audio.sink.clear();
  auto ctx = make_sink_context();

  const auto sink = audio::select_sink_name(ctx, 2, true);

  EXPECT_EQ(sink, "alsa_output.host");
  EXPECT_FALSE(audio::sink_is_virtual(ctx, sink));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, true));
  EXPECT_FALSE(audio::should_claim_default_sink(ctx, sink, true));

  config::audio.sink = old_sink;
}

TEST(AudioSinkSelectionTest, PrefersVirtualStreamSinkOverEasyEffectsDefault) {
  auto old_sink = config::audio.sink;
  config::audio.sink.clear();
  auto ctx = make_sink_context();
  ctx.sink.host = "easyeffects_sink";

  const auto sink = audio::select_sink_name(ctx, 6, false);

  EXPECT_EQ(sink, "sink-sunshine-surround51");
  EXPECT_TRUE(audio::host_sink_is_processing(ctx.sink.host));
  EXPECT_TRUE(audio::sink_is_virtual(ctx, sink));
#ifdef __linux__
  EXPECT_TRUE(audio::should_claim_default_sink(ctx, sink, false));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
#endif

  config::audio.sink = old_sink;
}

TEST(AudioSinkSelectionTest, ExplicitConfiguredSinkIsEnforced) {
  auto old_sink = config::audio.sink;
  config::audio.sink = "alsa_output.configured";
  auto ctx = make_sink_context();

  const auto sink = audio::select_sink_name(ctx, 2, false);

  // Web UI / conf audio_sink wins over auto virtual isolation.
  EXPECT_EQ(sink, "alsa_output.configured");
  EXPECT_FALSE(audio::sink_is_virtual(ctx, sink));
#ifdef __linux__
  EXPECT_TRUE(audio::should_claim_default_sink(ctx, sink, false));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
#else
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
#endif

  config::audio.sink = old_sink;
}

TEST(AudioSinkSelectionTest, ExplicitVirtualSinkIsEnforcedAndClaimed) {
  auto old_sink = config::audio.sink;
  config::audio.sink = "sink-sunshine-surround51";
  auto ctx = make_sink_context();

  const auto sink = audio::select_sink_name(ctx, 2, false);

  EXPECT_EQ(sink, "sink-sunshine-surround51");
  EXPECT_TRUE(audio::sink_is_virtual(ctx, sink));
#ifdef __linux__
  EXPECT_TRUE(audio::should_claim_default_sink(ctx, sink, false));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
#endif

  config::audio.sink = old_sink;
}

TEST(AudioSinkClaimOwnershipTest, OnlyASessionThatClaimedMayReleaseTheClaim) {
  // The claim is refcounted across sessions on one shared control. A session
  // that took the legacy set_sink path also sets restore_sink, so keying the
  // release off restore_sink would let it decrement a claim it never took —
  // and on the last decrement, restore the host default underneath a stream
  // that is still running.
  auto ctx = make_sink_context();
  ctx.control = std::make_unique<fake_audio_control_t>();

  ctx.restore_sink = false;
  ctx.claimed_default = false;
  EXPECT_FALSE(audio::owns_default_sink_claim(ctx));

  // Legacy path: something to undo at stop, but no claim was taken.
  ctx.restore_sink = true;
  ctx.claimed_default = false;
  EXPECT_FALSE(audio::owns_default_sink_claim(ctx));

  // Claim path.
  ctx.restore_sink = true;
  ctx.claimed_default = true;
  EXPECT_TRUE(audio::owns_default_sink_claim(ctx));
}

TEST(AudioSinkClaimOwnershipTest, AClaimWithoutAControlIsNotOwned) {
  // stop runs after the control is gone on some teardown paths; dereferencing
  // it to release would be worse than skipping the restore.
  auto ctx = make_sink_context();
  ctx.control.reset();
  ctx.restore_sink = true;
  ctx.claimed_default = true;

  EXPECT_FALSE(audio::owns_default_sink_claim(ctx));
}

TEST(AudioCaptureBufferDiagnosticsTest, KeepsSixtyMillisecondsOfStereoJitterForFiveMsPackets) {
  constexpr std::uint32_t frame_size = 240;  // 5 ms at 48 kHz
  constexpr int channels = 2;

  EXPECT_EQ(audio::capture_jitter_buffer_capacity(frame_size, channels), frame_size * channels * 12);
}

TEST(AudioCaptureBufferDiagnosticsTest, ReportsSamplesDroppedWhenIncomingBlockOverflows) {
  EXPECT_EQ(audio::ring_buffer_overflow_samples(8, 10, 5), 3);
}

TEST(AudioCaptureBufferDiagnosticsTest, DoesNotReportDropsWhenIncomingBlockFits) {
  EXPECT_EQ(audio::ring_buffer_overflow_samples(4, 10, 5), 0);
}
