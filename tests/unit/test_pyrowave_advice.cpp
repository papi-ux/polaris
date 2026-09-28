/**
 * @file tests/unit/test_pyrowave_advice.cpp
 * @brief PyroWave's bitrate model, the request it becomes on the wire, and the fixture that pins the
 *        host's figures.
 */
#include "src/crypto.h"
#include "src/pyrowave_advice.h"
#include "src/stream_bitrate.h"
#include "src/utility.h"
#if defined(POLARIS_BUILD_PYROWAVE) && defined(__linux__)
  #include "src/platform/linux/pyrowave_encode.h"
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
  std::string read_source_file(const std::filesystem::path &relative) {
    std::ifstream input(std::filesystem::path(POLARIS_SOURCE_DIR) / relative, std::ios::binary);
    std::stringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  nlohmann::json rate_model_fixture() {
    return nlohmann::json::parse(read_source_file("tests/fixtures/pyrowave-rate-model.json"));
  }

  std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return text;
  }
}  // namespace

TEST(StreamBitrateTests, EncoderRateIsTheRequestLessFecAudioAndOverhead) {
  // 10% FEC leaves 18000, stereo in high quality takes 512 and overhead 500.
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(20000, 10, 512), 16988);
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(180000, 10, 512), 160988);
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(300000, 10, 512), 268988);
  // A small request loses at most a fifth to audio and a tenth to overhead.
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(1000, 10, 512), 648);
  // FEC above 80% is not taken off at all.
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(20000, 90, 512), 18988);
  EXPECT_EQ(stream_bitrate::audio_kbps(true, 2), 512);
  EXPECT_EQ(stream_bitrate::audio_kbps(false, 8), 768);
}

TEST(StreamBitrateTests, TheRequestForAnEncoderRateIsTheSmallestThatReachesIt) {
  for (const int fec : {0, 10, 20, 50, 80, 90}) {
    for (const int audio : {192, 512, 2048}) {
      for (const int encoder : {1, 648, 5000, 16988, 76785, 153571, 179492, 268988, 400000}) {
        SCOPED_TRACE("fec " + std::to_string(fec) + ", audio " + std::to_string(audio) + ", encoder " +
                     std::to_string(encoder));
        const auto wire = stream_bitrate::wire_kbps_for_encoder(encoder, fec, audio);
        EXPECT_GE(stream_bitrate::encoder_kbps_for_wire(wire, fec, audio), encoder);
        EXPECT_LT(stream_bitrate::encoder_kbps_for_wire(wire - 1, fec, audio), encoder);
      }
    }
  }
  EXPECT_EQ(stream_bitrate::wire_kbps_for_encoder(0, 10, 512), 0);
  EXPECT_EQ(stream_bitrate::wire_kbps_for_encoder(-5, 10, 512), 0);
}

TEST(PyroWaveAdviceTests, TheModelMatchesTheFixtureAtTheRevisionItNames) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave, so there is no model to compare";
  }
  const auto fixture = rate_model_fixture();
  const auto revision = fixture.at("pyrowave_revision").get<std::string>();
  EXPECT_EQ(revision, pyrowave_advice::k_model_revision);
#if defined(POLARIS_BUILD_PYROWAVE) && defined(__linux__)
  // The fixture is keyed by the revision the codec's own profile token names, so moving the pin
  // without regenerating the figures on both ends fails here rather than on a client.
  EXPECT_NE(std::string {pyrowave_encode::profile_token}.find("pyrowave-" + revision.substr(0, 8) + "-"),
            std::string::npos)
    << pyrowave_encode::profile_token;
#endif
  const auto header = read_source_file(
    std::filesystem::path("third-party/pyrowave") / fixture.at("header").get<std::string>());
  ASSERT_FALSE(header.empty());
  EXPECT_EQ(lower(util::hex(crypto::hash(header), true).to_string()),
            fixture.at("header_sha256").get<std::string>());
  EXPECT_EQ(fixture.at("target_db"), pyrowave_advice::k_target_db);

  ASSERT_GE(fixture.at("cases").size(), 10u);
  for (const auto &row : fixture.at("cases")) {
    SCOPED_TRACE(row.dump());
    const auto got = pyrowave_advice::estimate(
      row.at("width"), row.at("height"), row.at("fps"), row.at("chroma") == "444", row.at("height_factor"));
    const double expected = row.at("mbps");
    EXPECT_NEAR(got.mbps, expected, expected * 1e-12);
    EXPECT_NEAR(got.kbps, static_cast<int>(expected * 1000.0), 1);
    EXPECT_EQ(pyrowave_advice::rule_name(got.rule), row.at("rule").get<std::string>());
  }
}

TEST(PyroWaveAdviceTests, TheAdviceIsARequestThatLandsTheEncoderOnTheModel) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  const pyrowave_advice::link_t link {10, 512};
  const auto advice = pyrowave_advice::advise(1920, 1080, 60, false, link, 0);
  ASSERT_TRUE(advice.valid);
  EXPECT_EQ(advice.far_encoder_kbps, 153571);
  EXPECT_EQ(advice.near_encoder_kbps, 220301);
  // The far figure is the lower one: a device's own screen is watched from farther away.
  EXPECT_EQ(advice.advice_far_kbps, 171759);
  EXPECT_EQ(advice.advice_near_kbps, 245904);
  EXPECT_LT(advice.advice_far_kbps, advice.advice_near_kbps);
  for (const auto &[wire, encoder] : {std::pair {advice.advice_far_kbps, advice.far_encoder_kbps},
                                      std::pair {advice.advice_near_kbps, advice.near_encoder_kbps}}) {
    EXPECT_GE(stream_bitrate::encoder_kbps_for_wire(wire, 10, 512), encoder);
    EXPECT_LT(stream_bitrate::encoder_kbps_for_wire(wire - 1, 10, 512), encoder);
  }
  EXPECT_EQ(advice.raise_goal_kbps, advice.advice_far_kbps);
  EXPECT_EQ(advice.raise_goal_limited_by, "advice");
  EXPECT_EQ(advice.raise_goal_encoder_kbps, 153571);
  EXPECT_EQ(advice.cap_kbps, 300000);
  // Live Tuning's floor is half the far figure, at the encoder.
  EXPECT_EQ(advice.floor_encoder_kbps, 76785);

  // More FEC and 7.1 audio ask for more on the wire for the same encoder rate.
  const auto surround = pyrowave_advice::advise(1920, 1080, 60, false, {20, 2048}, 0);
  EXPECT_EQ(surround.far_encoder_kbps, advice.far_encoder_kbps);
  EXPECT_GT(surround.advice_far_kbps, advice.advice_far_kbps);
}

TEST(PyroWaveAdviceTests, TheRaiseGoalStopsAtTheCapAndAtMaxBitrate) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  // 1080p120 in 4:4:4 on a device's own screen asks about 359 Mbps at the encoder, 400 on the wire.
  const auto high = pyrowave_advice::advise(1920, 1080, 120, true, {10, 512}, 0);
  ASSERT_TRUE(high.valid);
  EXPECT_EQ(high.advice_far_kbps, 399996);
  EXPECT_EQ(high.raise_goal_kbps, 300000);
  EXPECT_EQ(high.raise_goal_limited_by, "cap");
  EXPECT_EQ(high.raise_goal_encoder_kbps, 268988);

  const auto capped = pyrowave_advice::advise(1920, 1080, 60, false, {10, 512}, 50000);
  EXPECT_EQ(capped.raise_goal_kbps, 50000);
  EXPECT_EQ(capped.raise_goal_limited_by, "max_bitrate");
  EXPECT_EQ(capped.raise_goal_encoder_kbps, 43987);
  // The advice itself is not cut by the host's cap, only the raise.
  EXPECT_EQ(capped.advice_far_kbps, 171759);

  const auto roomy = pyrowave_advice::advise(1920, 1080, 60, false, {10, 512}, 250000);
  EXPECT_EQ(roomy.raise_goal_kbps, 171759);
  EXPECT_EQ(roomy.raise_goal_limited_by, "advice");
}

TEST(PyroWaveAdviceTests, AShapeThatIsNoStreamGetsNoAdvice) {
  EXPECT_FALSE(pyrowave_advice::advise(0, 1080, 60, false, {}, 0).valid);
  EXPECT_FALSE(pyrowave_advice::advise(1920, 1080, 0, false, {}, 0).valid);
  EXPECT_EQ(pyrowave_advice::estimate(1920, -1, 60, true, 15).rule, pyrowave_advice::rule_e::none);
}

TEST(PyroWaveAdviceTests, TheAdviceObjectCarriesTheContractFields) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  const auto json = pyrowave_advice::advice_json(pyrowave_advice::advise(1280, 800, 60, true, {10, 512}, 0));
  for (const auto *key : {"version", "model", "target_db", "width", "height", "fps", "chroma", "advice_far_kbps",
                          "advice_near_kbps", "raise_goal_kbps", "cap_kbps"}) {
    EXPECT_TRUE(json.contains(key)) << key;
  }
  EXPECT_EQ(json.at("version"), 1);
  EXPECT_EQ(json.at("model"), "psnr-hvs-m");
  EXPECT_EQ(json.at("target_db"), 35);
  EXPECT_EQ(json.at("chroma"), "444");
  EXPECT_EQ(json.at("rule"), "model_not_16_9");
  EXPECT_EQ(json.at("advice_far_kbps"), 160208);
}

TEST(PyroWaveAdviceTests, ThePreLaunchRouteChecksItsQueryAndSaysWhyPyroWaveIsUnavailable) {
  pyrowave_advice::route_host_t host;
  host.built = true;
  host.device_available = true;
  host.fec_percentage = 10;

  struct query_t {
    const char *width;
    const char *height;
    const char *fps;
    const char *chroma;
  };
  for (const auto &query : {query_t {"", "1080", "60", "420"}, query_t {"1920", "1080", "60", "422"},
                            query_t {"-1920", "1080", "60", "420"}, query_t {"1920", "1080", "0", "420"},
                            query_t {"1920x", "1080", "60", "420"}, query_t {"99999", "1080", "60", "420"},
                            query_t {"1920", "1080", "1001", "444"}, query_t {"1920", "", "60", "444"}}) {
    SCOPED_TRACE(std::string {query.width} + "x" + query.height + "@" + query.fps + " " + query.chroma);
    int status = 0;
    const auto reply = pyrowave_advice::advice_reply(query.width, query.height, query.fps, query.chroma, host, status);
    EXPECT_EQ(status, 400);
    EXPECT_EQ(reply.at("code"), "invalid_query");
  }

  // A build without PyroWave says so, and gives no figures.
  auto unbuilt = host;
  unbuilt.built = false;
  int status = 0;
  const auto not_built = pyrowave_advice::advice_reply("1920", "1080", "60", "420", unbuilt, status);
  EXPECT_EQ(status, 200);
  EXPECT_FALSE(not_built.at("available").get<bool>());
  EXPECT_EQ(not_built.at("reason"), "not_built");
  EXPECT_FALSE(not_built.contains("advice_far_kbps"));

  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  const auto served = pyrowave_advice::advice_reply("1920", "1080", "60", "420", host, status);
  EXPECT_EQ(status, 200);
  EXPECT_TRUE(served.at("available").get<bool>());
  EXPECT_FALSE(served.contains("reason"));
  EXPECT_EQ(served.at("advice_far_kbps"), 171759);
  EXPECT_EQ(served.at("advice_near_kbps"), 245904);
  EXPECT_EQ(served.at("assumes").at("fec_percentage"), 10);
  EXPECT_EQ(served.at("assumes").at("audio_kbps"), 512);

  // The figures do not depend on the device, so a host without one still gives them.
  auto deviceless = host;
  deviceless.device_available = false;
  const auto no_device = pyrowave_advice::advice_reply("1920", "1080", "60", "444", deviceless, status);
  EXPECT_EQ(status, 200);
  EXPECT_FALSE(no_device.at("available").get<bool>());
  EXPECT_EQ(no_device.at("reason"), "no_vulkan_device");
  EXPECT_EQ(no_device.at("advice_far_kbps"), 200561);

  auto capped = host;
  capped.max_bitrate_kbps = 50000;
  const auto limited = pyrowave_advice::advice_reply("1920", "1080", "60", "420", capped, status);
  EXPECT_EQ(limited.at("raise_goal_kbps"), 50000);
  EXPECT_EQ(limited.at("raise_goal_limited_by"), "max_bitrate");
}
