/**
 * @file tests/unit/test_codec_support_readout.cpp
 * @brief The 4:4:4 and PyroWave objects GET /api/config serves in encoder_codec_support.
 *
 * The console's codec panel had no row for either. A host could not see that its GPU encoder streams
 * 4:2:0 only, or whether it can serve PyroWave and why not, without starting a stream from a client.
 */
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/codec_support_readout.h"

namespace pa = pyrowave_availability;
namespace readout = codec_support_readout;

namespace {
  std::set<std::string> keys_of(const nlohmann::json &object) {
    std::set<std::string> keys;
    for (const auto &item : object.items()) {
      keys.insert(item.key());
    }
    return keys;
  }

  std::string source(const char *relative) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative);
    EXPECT_TRUE(input.is_open()) << relative;
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
  }

  const std::vector<readout::encoder_yuv444_t> linux_encoders {
    {"nvenc", {false, false, false}},
    {"vulkan", {false, false, false}},
    {"vaapi", {false, false, false}},
    {"software", {true, false, false}},
  };
}  // namespace

TEST(CodecSupportReadout, Yuv444NamesOnlyWhatClientsAreOffered) {
  // The probe passed all three in 4:4:4, but HEVC is switched off and AV1 did not pass, so serverinfo
  // offers neither in 4:4:4 and the console must not either.
  const auto value = readout::yuv444_json(1, 0, {true, true, true}, linux_encoders);
  EXPECT_TRUE(value.at("h264").get<bool>());
  EXPECT_FALSE(value.at("hevc").get<bool>());
  EXPECT_FALSE(value.at("av1").get<bool>());

  const auto offered = readout::yuv444_json(3, 2, {false, true, true}, linux_encoders);
  EXPECT_FALSE(offered.at("h264").get<bool>());
  EXPECT_TRUE(offered.at("hevc").get<bool>());
  EXPECT_TRUE(offered.at("av1").get<bool>());

  // A GPU encoder on Linux: nothing in 4:4:4, whatever the codec modes.
  const auto gpu = readout::yuv444_json(3, 3, {false, false, false}, linux_encoders);
  EXPECT_FALSE(gpu.at("h264").get<bool>());
  EXPECT_FALSE(gpu.at("hevc").get<bool>());
  EXPECT_FALSE(gpu.at("av1").get<bool>());
}

TEST(CodecSupportReadout, Yuv444ListsEveryEncoderInOrderWithTheCodecsItCanCarry) {
  const auto value = readout::yuv444_json(3, 2, {false, false, false}, linux_encoders);
  EXPECT_EQ(keys_of(value), (std::set<std::string> {"h264", "hevc", "av1", "encoders"}));
  const auto expected = nlohmann::json::parse(R"([
    {"encoder": "nvenc", "codecs": []},
    {"encoder": "vulkan", "codecs": []},
    {"encoder": "vaapi", "codecs": []},
    {"encoder": "software", "codecs": ["h264"]}
  ])");
  EXPECT_EQ(value.at("encoders"), expected);

  const auto all = readout::yuv444_json(3, 3, {true, true, true}, {{"nvenc", {true, true, true}}});
  EXPECT_EQ(all.at("encoders"), nlohmann::json::parse(R"([{"encoder": "nvenc", "codecs": ["h264", "hevc", "av1"]}])"));
}

TEST(CodecSupportReadout, PyroWaveObjectHasOneShape) {
  // The console reads every key, so each is present in every state, null when it does not apply.
  const std::set<std::string> shape {"available", "hdr", "reason", "message", "host_mode_refusal"};
  EXPECT_EQ(keys_of(readout::pyrowave_json(std::nullopt, true, std::nullopt)), shape);
  EXPECT_EQ(keys_of(readout::pyrowave_json(pa::unavailable({}), false, std::nullopt)), shape);
  EXPECT_EQ(keys_of(readout::pyrowave_json(std::nullopt, false, pa::launch_refusal(pa::route_e::fp16_scanout))), shape);
}

TEST(CodecSupportReadout, UnavailablePyroWaveSaysWhatCapabilitiesSaysWordForWord) {
  pa::offer_facts_t facts;
  for (const auto &[built, device] : std::vector<std::pair<bool, bool>> {{false, false}, {true, false}}) {
    facts.built = built;
    facts.device = device;
    const auto unavailable = pa::unavailable(facts);
    ASSERT_TRUE(unavailable.has_value());
    // hdr and a refusal passed in anyway must not leak into an answer that says it is unavailable.
    const auto value = readout::pyrowave_json(unavailable, true, pa::launch_refusal(pa::route_e::fp16_scanout));
    EXPECT_FALSE(value.at("available").get<bool>());
    EXPECT_FALSE(value.at("hdr").get<bool>());
    EXPECT_EQ(value.at("reason").get<std::string>(), pa::reason_id(unavailable->reason));
    EXPECT_EQ(value.at("message").get<std::string>(), unavailable->message);
    EXPECT_TRUE(value.at("host_mode_refusal").is_null());
  }

  facts.built = true;
  facts.device = true;
  facts.private_mode_available = false;
  facts.host_route = pa::route_e::fp16_scanout;
  const auto value = readout::pyrowave_json(pa::unavailable(facts), false, std::nullopt);
  EXPECT_EQ(value.at("reason").get<std::string>(), "fp16_capture");
  EXPECT_NE(value.at("message").get<std::string>().find("sixteen bit float"), std::string::npos);
}

TEST(CodecSupportReadout, AvailablePyroWaveHasNoReasonAndCarriesHdr) {
  const auto hdr = readout::pyrowave_json(std::nullopt, true, std::nullopt);
  EXPECT_TRUE(hdr.at("available").get<bool>());
  EXPECT_TRUE(hdr.at("hdr").get<bool>());
  EXPECT_TRUE(hdr.at("reason").is_null());
  EXPECT_TRUE(hdr.at("message").is_null());
  EXPECT_TRUE(hdr.at("host_mode_refusal").is_null());

  EXPECT_FALSE(readout::pyrowave_json(std::nullopt, false, std::nullopt).at("hdr").get<bool>());
}

TEST(CodecSupportReadout, PyroWaveOfferedOnlyToPrivateModesSaysWhatTheHostModeLaunchGets) {
  // pc-papi's shape: KDE in HDR, capture = kms, and Private Stream on PATH. Capabilities offers
  // PyroWave, and a launch into the host's own mode is refused at launch. The console says both.
  pa::offer_facts_t facts {true, true, pa::route_e::fp16_scanout, true};
  ASSERT_FALSE(pa::unavailable(facts).has_value());
  const auto refusal = pa::launch_refusal(pa::route_e::fp16_scanout);
  ASSERT_TRUE(refusal.has_value());

  const auto value = readout::pyrowave_json(pa::unavailable(facts), false, refusal);
  EXPECT_TRUE(value.at("available").get<bool>());
  const auto &own = value.at("host_mode_refusal");
  EXPECT_EQ(keys_of(own), (std::set<std::string> {"code", "message", "action"}));
  EXPECT_EQ(own.at("code").get<std::string>(), "pyrowave_capture_unreadable");
  EXPECT_EQ(own.at("message").get<std::string>(), refusal->message);
  EXPECT_EQ(own.at("action").get<std::string>(), refusal->action);
}

TEST(CodecSupportReadout, ConfigServesBothObjectsInsideEncoderCodecSupport) {
  // The builders are only worth their tests if the route uses them, with the answers capabilities
  // uses, so the wiring is pinned here.
  const auto confighttp = source("src/confighttp.cpp");
  const auto begin = confighttp.find("output_tree[\"encoder_codec_support\"] = nlohmann::json {");
  ASSERT_NE(begin, std::string::npos);
  const auto end = confighttp.find("};", begin);
  ASSERT_NE(end, std::string::npos);
  const auto object = confighttp.substr(begin, end - begin);
  EXPECT_NE(object.find("{\"yuv444\", codec_support_readout::yuv444_json("), std::string::npos);
  EXPECT_NE(object.find("codec_state.yuv444_for_codec"), std::string::npos);
  EXPECT_NE(object.find("video::yuv444_encoders()"), std::string::npos);
  EXPECT_NE(object.find("{\"pyrowave\", codec_support_readout::pyrowave_json("), std::string::npos);
  EXPECT_NE(object.find("video::pyrowave_host_mode_refusal()"), std::string::npos);
  EXPECT_NE(confighttp.find("const auto pyrowave_unavailable = video::pyrowave_unavailable();"), std::string::npos);
}
