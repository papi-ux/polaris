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

TEST(AudioSinkSelectionTest, SelectsVirtualSinkWhenHostAudioIsDisabled) {
  auto old_sink = config::audio.sink;
  config::audio.sink.clear();
  auto ctx = make_sink_context();

  const auto sink = audio::select_sink_name(ctx, 2, false);

  EXPECT_EQ(sink, "sink-sunshine-stereo");
  EXPECT_TRUE(audio::sink_is_virtual(ctx, sink));
#ifdef __linux__
  EXPECT_TRUE(audio::should_route_session_sink_without_default(ctx, sink, false));
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

  config::audio.sink = old_sink;
}

TEST(AudioSinkSelectionTest, ExplicitConfiguredSinkKeepsDefaultRoutingBehavior) {
  auto old_sink = config::audio.sink;
  config::audio.sink = "alsa_output.configured";
  auto ctx = make_sink_context();

  const auto sink = audio::select_sink_name(ctx, 2, false);

  EXPECT_EQ(sink, "sink-sunshine-stereo");
  EXPECT_TRUE(audio::sink_is_virtual(ctx, sink));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));

  config::audio.sink = old_sink;
}
