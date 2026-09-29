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
#include <utility>

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

  /// Whether estimate() takes these arguments. A template, so a call it refuses reads as false here
  /// rather than failing the build.
  template<typename... Args>
  concept estimate_takes = requires(Args... args) { pyrowave_advice::estimate(args...); };

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

TEST(StreamBitrateTests, AClientMaySetUpTo500MbpsByHandAndTheHostRecommendsNoMoreThan300) {
  EXPECT_EQ(stream_bitrate::k_min_request_kbps, 1000);
  EXPECT_EQ(stream_bitrate::k_max_request_kbps, 500000);
  EXPECT_FALSE(stream_bitrate::request_in_range(-1));
  EXPECT_FALSE(stream_bitrate::request_in_range(0));
  EXPECT_FALSE(stream_bitrate::request_in_range(999));
  EXPECT_TRUE(stream_bitrate::request_in_range(1000));
  EXPECT_TRUE(stream_bitrate::request_in_range(300001));
  EXPECT_TRUE(stream_bitrate::request_in_range(500000));
  EXPECT_FALSE(stream_bitrate::request_in_range(500001));
  EXPECT_EQ(stream_bitrate::request_range_text(), "between 1000 and 500000");
  // A 500 Mbps request lands the encoder on 448987 kbps at 10% FEC with stereo.
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(500000, 10, 512), 448987);

  // Doctor's raise and every figure the host recommends stay at 300 Mbps, below what a player may set.
  EXPECT_EQ(pyrowave_advice::k_cap_kbps, 300000);
  EXPECT_LT(pyrowave_advice::k_cap_kbps, stream_bitrate::k_max_request_kbps);
}

TEST(StreamBitrateTests, SplittingARequestRecordsWhatItWasSplitFor) {
  stream_bitrate::request_t request;
  const auto encoder = stream_bitrate::split_request(request, 20000, 10, 512);
  ASSERT_TRUE(encoder.has_value());
  EXPECT_EQ(*encoder, 16988);
  EXPECT_EQ(*encoder, stream_bitrate::encoder_kbps_for_wire(20000, 10, 512));
  EXPECT_EQ(request.split_kbps, 20000);
  EXPECT_EQ(request.encoder_kbps, 16988);
  EXPECT_EQ(request.audio_kbps, 512);
  EXPECT_EQ(request.fec_percentage, 10);

  // No total splits nothing, and the audio and FEC are recorded anyway.
  stream_bitrate::request_t unsplit;
  EXPECT_FALSE(stream_bitrate::split_request(unsplit, 0, 20, 1536).has_value());
  EXPECT_EQ(unsplit.split_kbps, 0);
  EXPECT_EQ(unsplit.encoder_kbps, 0);
  EXPECT_EQ(unsplit.audio_kbps, 1536);
  EXPECT_EQ(unsplit.fec_percentage, 20);
  EXPECT_EQ(stream_bitrate::k_formula, "stream_bitrate_v1");
}

TEST(StreamBitrateTests, AWatcherEncodesAtItsOwnersRateWhichIsNoSplitOfItsRequest) {
  // Any other stream starts where its split left the encoder, and keeps its split.
  stream_bitrate::request_t own;
  const auto encoder = stream_bitrate::split_request(own, 50000, 10, 512);
  ASSERT_TRUE(encoder.has_value());
  EXPECT_EQ(stream_bitrate::settle_encoder(own, static_cast<int>(*encoder), std::nullopt), 43987);
  EXPECT_EQ(own.split_kbps, 50000);
  EXPECT_EQ(own.encoder_kbps, 43987);

  // A watcher starts at its owner's rate, and the split of its own request is cleared, because the
  // formula says nothing about that rate. Its audio and FEC stay.
  stream_bitrate::request_t watcher;
  stream_bitrate::split_request(watcher, 50000, 10, 512);
  EXPECT_EQ(stream_bitrate::settle_encoder(watcher, 43987, 20000), 20000);
  EXPECT_EQ(watcher.split_kbps, 0);
  EXPECT_EQ(watcher.encoder_kbps, 20000);
  EXPECT_EQ(watcher.audio_kbps, 512);
  EXPECT_EQ(watcher.fec_percentage, 10);

  // A client that sent no bitrate starts where it asked, 0, with nothing split.
  stream_bitrate::request_t unsplit;
  EXPECT_FALSE(stream_bitrate::split_request(unsplit, 0, 10, 512).has_value());
  EXPECT_EQ(stream_bitrate::settle_encoder(unsplit, 0, std::nullopt), 0);
  EXPECT_EQ(unsplit.split_kbps, 0);
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
      row.at("width"), row.at("height"), row.at("fps"), row.at("chroma") == "444", row.at("height_factor"),
      pyrowave_advice::k_target_db);
    const double expected = row.at("mbps");
    EXPECT_NEAR(got.mbps, expected, expected * 1e-12);
    EXPECT_NEAR(got.kbps, static_cast<int>(expected * 1000.0), 1);
    EXPECT_EQ(pyrowave_advice::rule_name(got.rule), row.at("rule").get<std::string>());
  }

  // The far figure's rows, at the calibrated handheld target Nova mirrors from nova#130.
  EXPECT_EQ(fixture.at("far_target_db"), pyrowave_advice::k_far_target_db);
  ASSERT_GE(fixture.at("far_cases").size(), 10u);
  for (const auto &row : fixture.at("far_cases")) {
    SCOPED_TRACE(row.dump());
    EXPECT_EQ(row.at("height_factor"), pyrowave_advice::k_height_factor_far);
    const auto got = pyrowave_advice::estimate(
      row.at("width"), row.at("height"), row.at("fps"), row.at("chroma") == "444", row.at("height_factor"),
      pyrowave_advice::k_far_target_db);
    const double expected = row.at("mbps");
    EXPECT_NEAR(got.mbps, expected, expected * 1e-12);
    EXPECT_NEAR(got.kbps, static_cast<int>(expected * 1000.0), 1);
    EXPECT_EQ(pyrowave_advice::rule_name(got.rule), row.at("rule").get<std::string>());
  }
}

TEST(PyroWaveAdviceTests, EveryEstimateNamesItsTarget) {
  // The far and near figures aim for different qualities, so an estimate with no target would be the
  // television's 35 dB wherever a far figure forgot to ask for 31.
  static_assert(estimate_takes<int, int, int, bool, int, int>);
  static_assert(!estimate_takes<int, int, int, bool, int>,
                "estimate() must not default its target: name k_far_target_db or k_target_db");
  SUCCEED();
}

TEST(PyroWaveAdviceTests, TheOwnScreenFigureIsCalibratedAndTheTelevisionFigureKeeps35Db) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  EXPECT_EQ(pyrowave_advice::k_far_target_db, 31);
  EXPECT_EQ(pyrowave_advice::k_target_db, 35);

  // The calibration: 200 Mbps was right on a Retroid Pocket 6 at 1920x1080, 120 fps and 4:4:4, where
  // the model at 35 dB asks 359 Mbps at the encoder, 400 as a request. At 31 dB it asks 192 and 215.
  EXPECT_EQ(pyrowave_advice::estimate(1920, 1080, 120, true, pyrowave_advice::k_height_factor_far, 35).kbps, 358984);
  EXPECT_EQ(pyrowave_advice::estimate(1920, 1080, 120, true, pyrowave_advice::k_height_factor_far, 31).kbps, 192396);

  struct row_t {
    int width;
    int height;
    int fps;
    bool chroma444;
    int far_encoder;
    int far_request;
    int near_encoder;
    int near_request;
  };

  const pyrowave_advice::link_t link {10, 512};
  for (const auto &row : {
         row_t {1920, 1080, 60, false, 89121, 100148, 220301, 245904},
         row_t {1920, 1080, 60, true, 96198, 108012, 266744, 297507},
         row_t {1920, 1080, 120, false, 178243, 199173, 440602, 490683},
         row_t {1920, 1080, 120, true, 192396, 214898, 533489, 593890},
         row_t {2560, 1440, 120, false, 213540, 238392, 578464, 643863},
         row_t {2560, 1440, 120, true, 224169, 250202, 683648, 760734},
         row_t {3840, 2160, 60, false, 143036, 160054, 291016, 324476},
         row_t {3840, 2160, 60, true, 146947, 164399, 313803, 349795},
         row_t {3840, 2160, 120, false, 286073, 318984, 582033, 647828},
         row_t {3840, 2160, 120, true, 293895, 327675, 627606, 698465},
       }) {
    SCOPED_TRACE(std::to_string(row.width) + "x" + std::to_string(row.height) + " at " + std::to_string(row.fps) +
                 (row.chroma444 ? " 4:4:4" : " 4:2:0"));
    const auto advice = pyrowave_advice::advise(row.width, row.height, row.fps, row.chroma444, link, 0);
    ASSERT_TRUE(advice.valid);
    // A device's own screen: the calibrated handheld target.
    EXPECT_EQ(advice.far_encoder_kbps, row.far_encoder);
    EXPECT_EQ(advice.advice_far_kbps, row.far_request);
    EXPECT_EQ(advice.far_encoder_kbps,
              pyrowave_advice::estimate(row.width, row.height, row.fps, row.chroma444,
                                        pyrowave_advice::k_height_factor_far, pyrowave_advice::k_far_target_db).kbps);
    // A television or monitor: the author's 35 dB, the figures it gave before the calibration.
    EXPECT_EQ(advice.near_encoder_kbps, row.near_encoder);
    EXPECT_EQ(advice.advice_near_kbps, row.near_request);
    EXPECT_EQ(advice.near_encoder_kbps,
              pyrowave_advice::estimate(row.width, row.height, row.fps, row.chroma444,
                                        pyrowave_advice::k_height_factor_near, pyrowave_advice::k_target_db).kbps);
    EXPECT_LT(advice.advice_far_kbps, advice.advice_near_kbps);
    // Doctor's raise goes to the far figure and never past the cap, and Live Tuning's floor is half it.
    EXPECT_EQ(advice.raise_goal_kbps, std::min(row.far_request, pyrowave_advice::k_cap_kbps));
    EXPECT_EQ(advice.floor_encoder_kbps, row.far_encoder / 2);
  }
}

TEST(PyroWaveAdviceTests, TheAdviceIsARequestThatLandsTheEncoderOnTheModel) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  const pyrowave_advice::link_t link {10, 512};
  const auto advice = pyrowave_advice::advise(1920, 1080, 60, false, link, 0);
  ASSERT_TRUE(advice.valid);
  EXPECT_EQ(advice.far_encoder_kbps, 89121);
  EXPECT_EQ(advice.near_encoder_kbps, 220301);
  // The far figure is the lower one: a device's own screen is watched from farther away, and it aims
  // for the calibrated 31 dB rather than 35.
  EXPECT_EQ(advice.advice_far_kbps, 100148);
  EXPECT_EQ(advice.advice_near_kbps, 245904);
  EXPECT_LT(advice.advice_far_kbps, advice.advice_near_kbps);
  for (const auto &[wire, encoder] : {std::pair {advice.advice_far_kbps, advice.far_encoder_kbps},
                                      std::pair {advice.advice_near_kbps, advice.near_encoder_kbps}}) {
    EXPECT_GE(stream_bitrate::encoder_kbps_for_wire(wire, 10, 512), encoder);
    EXPECT_LT(stream_bitrate::encoder_kbps_for_wire(wire - 1, 10, 512), encoder);
  }
  EXPECT_EQ(advice.raise_goal_kbps, advice.advice_far_kbps);
  EXPECT_EQ(advice.raise_goal_limited_by, "advice");
  EXPECT_EQ(advice.raise_goal_encoder_kbps, 89121);
  EXPECT_EQ(advice.cap_kbps, 300000);
  // Live Tuning's floor is half the far figure, at the encoder, and 51 Mbps as a request.
  EXPECT_EQ(advice.floor_encoder_kbps, 44560);
  EXPECT_EQ(pyrowave_advice::request_for_encoder(advice.floor_encoder_kbps, link), 50636);
  EXPECT_EQ(pyrowave_advice::request_for_encoder(advice.far_encoder_kbps, link), advice.advice_far_kbps);
  EXPECT_EQ(pyrowave_advice::request_for_encoder(0, link), 0);

  // More FEC and 7.1 audio ask for more on the wire for the same encoder rate.
  const auto surround = pyrowave_advice::advise(1920, 1080, 60, false, {20, 2048}, 0);
  EXPECT_EQ(surround.far_encoder_kbps, advice.far_encoder_kbps);
  EXPECT_GT(surround.advice_far_kbps, advice.advice_far_kbps);
}

TEST(PyroWaveAdviceTests, TheRaiseGoalStopsAtTheCapAndAtMaxBitrate) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  // 3840x2160 at 120 fps in 4:4:4 on a device's own screen asks about 294 Mbps at the encoder, 328 as
  // a request, past the cap. A player may set that by hand; Doctor raises to the cap and no further.
  EXPECT_EQ(pyrowave_advice::k_cap_kbps, 300000);
  const auto high = pyrowave_advice::advise(3840, 2160, 120, true, {10, 512}, 0);
  ASSERT_TRUE(high.valid);
  EXPECT_EQ(high.advice_far_kbps, 327675);
  EXPECT_EQ(high.raise_goal_kbps, 300000);
  EXPECT_EQ(high.raise_goal_limited_by, "cap");
  EXPECT_EQ(high.raise_goal_encoder_kbps, 268988);
  // A max_bitrate above the cap does not lift it.
  const auto roomier = pyrowave_advice::advise(3840, 2160, 120, true, {10, 512}, 450000);
  EXPECT_EQ(roomier.raise_goal_kbps, 300000);
  EXPECT_EQ(roomier.raise_goal_limited_by, "cap");

  // The Retroid Pocket 6 check, 1080p120 in 4:4:4, which the 35 dB figure put past the cap at 400,
  // now raises to its own figure.
  const auto rp6 = pyrowave_advice::advise(1920, 1080, 120, true, {10, 512}, 0);
  EXPECT_EQ(rp6.advice_far_kbps, 214898);
  EXPECT_EQ(rp6.raise_goal_kbps, 214898);
  EXPECT_EQ(rp6.raise_goal_limited_by, "advice");

  const auto capped = pyrowave_advice::advise(1920, 1080, 60, false, {10, 512}, 50000);
  EXPECT_EQ(capped.raise_goal_kbps, 50000);
  EXPECT_EQ(capped.raise_goal_limited_by, "max_bitrate");
  EXPECT_EQ(capped.raise_goal_encoder_kbps, 43987);
  // The advice itself is not cut by the host's cap, only the raise.
  EXPECT_EQ(capped.advice_far_kbps, 100148);

  const auto roomy = pyrowave_advice::advise(1920, 1080, 60, false, {10, 512}, 250000);
  EXPECT_EQ(roomy.raise_goal_kbps, 100148);
  EXPECT_EQ(roomy.raise_goal_limited_by, "advice");
}

TEST(PyroWaveAdviceTests, AStreamIsStarvedOnlyMoreThanATenthBelowTheRaiseGoal) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  EXPECT_DOUBLE_EQ(pyrowave_advice::k_starved_below_share, 0.9);
  const pyrowave_advice::link_t link {10, 512};
  const auto rp6 = pyrowave_advice::advise(1920, 1080, 120, true, link, 0);
  ASSERT_EQ(rp6.raise_goal_kbps, 214898);

  // The Retroid Pocket 6 at the 200 Mbps judged right is 93% of the goal, and healthy. The encoder
  // rate that request lands on reads back as the least request that lands there, a kbps under it.
  EXPECT_FALSE(pyrowave_advice::starved(rp6, 200000));
  EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(200000, 10, 512), 178987);
  EXPECT_EQ(pyrowave_advice::request_for_encoder(178987, link), 199999);
  EXPECT_FALSE(pyrowave_advice::starved(rp6, pyrowave_advice::request_for_encoder(178987, link)));

  // The line is 90% of the goal, 193408.2 kbps, in requests.
  EXPECT_FALSE(pyrowave_advice::starved(rp6, 193409));
  EXPECT_TRUE(pyrowave_advice::starved(rp6, 193408));
  EXPECT_TRUE(pyrowave_advice::starved(rp6, 150000));
  EXPECT_FALSE(pyrowave_advice::starved(rp6, 214898));
  EXPECT_FALSE(pyrowave_advice::starved(rp6, 450000));

  // The line follows the goal where the cap or max_bitrate holds it lower than the advice.
  const auto capped = pyrowave_advice::advise(3840, 2160, 120, true, link, 0);
  ASSERT_EQ(capped.raise_goal_kbps, 300000);
  EXPECT_FALSE(pyrowave_advice::starved(capped, 270000));
  EXPECT_TRUE(pyrowave_advice::starved(capped, 269999));
  const auto host_cap = pyrowave_advice::advise(1920, 1080, 60, false, link, 50000);
  ASSERT_EQ(host_cap.raise_goal_kbps, 50000);
  EXPECT_FALSE(pyrowave_advice::starved(host_cap, 45000));
  EXPECT_TRUE(pyrowave_advice::starved(host_cap, 44999));

  // No rate, or no advice, is no verdict.
  EXPECT_FALSE(pyrowave_advice::starved(rp6, 0));
  EXPECT_FALSE(pyrowave_advice::starved(pyrowave_advice::advise(0, 1080, 60, false, link, 0), 1000));
}

TEST(PyroWaveAdviceTests, AStarvedStreamAlwaysHasARaiseToOffer) {
  // Doctor's starved finding has no branch for a stream already at its raise goal, because there is
  // none: a request is starved only below 90% of the goal, a request never falls as the encoder rate
  // rises, and at the goal's own encoder rate it is not starved. So a starved stream's encoder always
  // runs below the goal's, which is where Doctor's raise goes.
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  for (const int fec : {0, 10, 20, 50, 80}) {
    for (const int audio : {512, 1536}) {
      const pyrowave_advice::link_t link {fec, audio};
      int previous = 0;
      for (int encoder = 1000; encoder <= 300000; encoder += 997) {
        const int request = pyrowave_advice::request_for_encoder(encoder, link);
        EXPECT_GE(request, previous) << "fec " << fec << " audio " << audio << " encoder " << encoder;
        previous = request;
      }
      for (const auto &[width, height] : std::initializer_list<std::pair<int, int>> {
             {1280, 720}, {1280, 800}, {1920, 1080}, {2560, 1440}, {3840, 2160}}) {
        for (const int fps : {30, 60, 120}) {
          for (const bool chroma444 : {false, true}) {
            for (const int max_bitrate : {0, 50000, 150000}) {
              const auto advice = pyrowave_advice::advise(width, height, fps, chroma444, link, max_bitrate);
              ASSERT_TRUE(advice.valid);
              SCOPED_TRACE(std::to_string(width) + "x" + std::to_string(height) + " at " + std::to_string(fps) +
                           " fec " + std::to_string(fec) + " audio " + std::to_string(audio) + " max_bitrate " +
                           std::to_string(max_bitrate));
              EXPECT_FALSE(pyrowave_advice::starved(
                advice, pyrowave_advice::request_for_encoder(advice.raise_goal_encoder_kbps, link)));
              EXPECT_FALSE(pyrowave_advice::starved(
                advice, pyrowave_advice::request_for_encoder(advice.raise_goal_encoder_kbps + 1, link)));
            }
          }
        }
      }
    }
  }
}

TEST(PyroWaveAdviceTests, AStreamNeedsMoreThanAllowedOnlyWhereTheHostHoldsItBelowTheModelAtItsCeiling) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  EXPECT_DOUBLE_EQ(pyrowave_advice::k_ceiling_bound_share, 0.8);
  const pyrowave_advice::link_t link {10, 512};

  // 3840x2160 at 120 fps in 4:4:4 on a device's own screen: the model asks 327675 kbps, and the cap
  // holds the goal at 300000. From 90% of the goal, where starved ends, to just below the model's
  // figure, a stream with more than 80% of its frames at the byte budget wants more than Doctor raises
  // it to.
  const auto capped = pyrowave_advice::advise(3840, 2160, 120, true, link, 0);
  ASSERT_EQ(capped.advice_far_kbps, 327675);
  ASSERT_EQ(capped.raise_goal_kbps, 300000);
  EXPECT_TRUE(pyrowave_advice::needs_more_than_allowed(capped, 300000, 0.85));
  EXPECT_TRUE(pyrowave_advice::needs_more_than_allowed(capped, 270000, 0.85));
  EXPECT_TRUE(pyrowave_advice::needs_more_than_allowed(capped, 327674, 0.85));
  // More than 80%, not 80%, and a share not yet known is no verdict.
  EXPECT_TRUE(pyrowave_advice::needs_more_than_allowed(capped, 300000, 0.801));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 300000, 0.8));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 300000, 0.2));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 300000, std::nullopt));
  // Below 90% of the goal the stream is starved, and Doctor's raise answers it instead.
  ASSERT_TRUE(pyrowave_advice::starved(capped, 269999));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 269999, 1.0));
  // At the model's figure or above it, set by hand, the model asks for nothing more.
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 327675, 1.0));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 500000, 1.0));

  // max_bitrate holds a stream below the model the same way.
  const auto host_cap = pyrowave_advice::advise(1920, 1080, 120, true, link, 200000);
  ASSERT_EQ(host_cap.advice_far_kbps, 214898);
  ASSERT_EQ(host_cap.raise_goal_limited_by, "max_bitrate");
  EXPECT_TRUE(pyrowave_advice::needs_more_than_allowed(host_cap, 200000, 0.85));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(host_cap, 200000, 0.2));

  // Where the goal is the model's own figure nothing holds the stream back, and a full budget there is
  // no finding: the Retroid Pocket 6 looked right at 200 Mbps against 215.
  const auto rp6 = pyrowave_advice::advise(1920, 1080, 120, true, link, 0);
  ASSERT_EQ(rp6.raise_goal_limited_by, "advice");
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(rp6, 200000, 1.0));
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(rp6, 214898, 1.0));

  // No rate, or no advice, is no verdict.
  EXPECT_FALSE(pyrowave_advice::needs_more_than_allowed(capped, 0, 1.0));
  EXPECT_FALSE(
    pyrowave_advice::needs_more_than_allowed(pyrowave_advice::advise(0, 1080, 60, false, link, 0), 1000, 1.0));
}

TEST(PyroWaveAdviceTests, AShapeThatIsNoStreamGetsNoAdvice) {
  EXPECT_FALSE(pyrowave_advice::advise(0, 1080, 60, false, {}, 0).valid);
  EXPECT_FALSE(pyrowave_advice::advise(1920, 1080, 0, false, {}, 0).valid);
  EXPECT_EQ(pyrowave_advice::estimate(1920, -1, 60, true, 15, pyrowave_advice::k_far_target_db).rule,
            pyrowave_advice::rule_e::none);
}

TEST(PyroWaveAdviceTests, TheAdviceObjectCarriesTheContractFields) {
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  const auto json = pyrowave_advice::advice_json(pyrowave_advice::advise(1280, 800, 60, true, {10, 512}, 0));
  for (const auto *key : {"version", "model", "target_db", "far_target_db", "width", "height", "fps", "chroma",
                          "advice_far_kbps", "advice_near_kbps", "raise_goal_kbps", "cap_kbps"}) {
    EXPECT_TRUE(json.contains(key)) << key;
  }
  EXPECT_EQ(json.at("version"), 1);
  EXPECT_EQ(json.at("model"), "psnr-hvs-m");
  EXPECT_EQ(json.at("target_db"), 35);
  EXPECT_EQ(json.at("far_target_db"), 31);
  EXPECT_EQ(json.at("chroma"), "444");
  EXPECT_EQ(json.at("rule"), "model_not_16_9");
  EXPECT_EQ(json.at("advice_far_kbps"), 113317);
  EXPECT_EQ(json.at("advice_near_kbps"), 198291);
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
  EXPECT_EQ(served.at("advice_far_kbps"), 100148);
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
  EXPECT_EQ(no_device.at("advice_far_kbps"), 108012);

  auto capped = host;
  capped.max_bitrate_kbps = 50000;
  const auto limited = pyrowave_advice::advice_reply("1920", "1080", "60", "420", capped, status);
  EXPECT_EQ(limited.at("raise_goal_kbps"), 50000);
  EXPECT_EQ(limited.at("raise_goal_limited_by"), "max_bitrate");
}

TEST(PyroWaveAdviceTests, AStreamsLinkIsTheOneItsHandshakeRecorded) {
  // No stream: the host's FEC share now and stereo in high quality.
  const auto none = pyrowave_advice::stream_link(nullptr, 15);
  EXPECT_EQ(none.fec_percentage, 15);
  EXPECT_EQ(none.audio_kbps, pyrowave_advice::k_default_audio_kbps);

  // A stream: the FEC share it started with, whatever the host's is now, and its own audio.
  stream_bitrate::request_t surround;
  stream_bitrate::split_request(surround, 80000, 20, stream_bitrate::audio_kbps(true, 6));
  const auto own = pyrowave_advice::stream_link(&surround, 10);
  EXPECT_EQ(own.fec_percentage, 20);
  EXPECT_EQ(own.audio_kbps, 1536);

  // A share of 0 is the stream's own too, not a missing one.
  stream_bitrate::request_t no_fec;
  stream_bitrate::split_request(no_fec, 80000, 0, stream_bitrate::audio_kbps(false, 2));
  const auto unprotected = pyrowave_advice::stream_link(&no_fec, 10);
  EXPECT_EQ(unprotected.fec_percentage, 0);
  EXPECT_EQ(unprotected.audio_kbps, 192);
}

TEST(PyroWaveAdviceTests, ThePreLaunchRouteAssumesTheAskingClientsOwnAudio) {
  // Before a launch the route assumes stereo in high quality. A client streaming here is answered for
  // its own stream's audio, which nvhttp puts in the host it passes.
  pyrowave_advice::route_host_t host;
  EXPECT_EQ(host.audio_kbps, pyrowave_advice::k_default_audio_kbps);
  EXPECT_EQ(host.audio_kbps, stream_bitrate::audio_kbps(true, 2));
  if (!pyrowave_advice::model_available()) {
    GTEST_SKIP() << "built without PyroWave";
  }
  host.built = true;
  host.device_available = true;
  host.fec_percentage = 10;
  host.audio_kbps = stream_bitrate::audio_kbps(true, 6);
  int status = 0;
  const auto surround = pyrowave_advice::advice_reply("1920", "1080", "60", "420", host, status);
  EXPECT_EQ(status, 200);
  EXPECT_EQ(surround.at("assumes").at("audio_kbps"), 1536);
  EXPECT_EQ(surround.at("assumes").at("fec_percentage"), 10);
  const auto advice = pyrowave_advice::advise(1920, 1080, 60, false, {10, 1536}, 0);
  EXPECT_EQ(surround.at("advice_far_kbps"), advice.advice_far_kbps);
  // The same encoder rate asks more with 5.1 around it than with stereo.
  EXPECT_GT(surround.at("advice_far_kbps").get<int>(), 100148);
}
