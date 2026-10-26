/**
 * @file tests/unit/test_stream_bitrate_units.cpp
 * @brief Literal bitrate-unit wire contract tests in the HTTP test target.
 */

#include <src/stream_bitrate.h>
#include <src/stream_stats.h>
#include <src/utility.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  // One stream's handshake as rtsp.cpp records it, for a request nothing warped or capped: the split of
  // the client's total, the audio and FEC it was split for, and where it left the encoder.
  stream_bitrate::request_t negotiated_request(std::int64_t total_kbps, int fec_percentage, int audio_kbps) {
    stream_bitrate::request_t request;
    request.client_kbps = total_kbps;
    const auto encoder = stream_bitrate::split_request(request, total_kbps, fec_percentage, audio_kbps);
    stream_bitrate::settle_encoder(request, static_cast<int>(encoder.value_or(0)), std::nullopt);
    return request;
  }

  // A status snapshot with recorded streams, the way get_current() copies them out.
  stream_stats::client_stats_t recorded_stream(std::uint64_t generation, const stream_bitrate::request_t &request) {
    stream_stats::client_stats_t client;
    client.session_generation = generation;
    client.bitrate_kbps = request.encoder_kbps;
    client.bitrate_request = request;
    client.bitrate_request_recorded = true;
    return client;
  }

  stream_stats::stats_t streaming_stats(std::vector<stream_stats::client_stats_t> clients) {
    stream_stats::stats_t stats {};
    stats.streaming = true;
    stats.clients = std::move(clients);
    return stats;
  }

  // The published figures satisfy the formula they name, both ways: the split lands the encoder on
  // encoder_kbps exactly, and the smallest request that reaches encoder_kbps is at most the split. The
  // split is the client's request times the warp factor, cut to the cap when there is one.
  void expect_round_trip(const nlohmann::json &units) {
    ASSERT_TRUE(units.is_object());
    ASSERT_EQ(units.at("formula"), "stream_bitrate_v1");
    ASSERT_TRUE(units.at("split_kbps").is_number_integer()) << units.dump();
    const auto requested = units.at("requested_kbps").get<std::int64_t>();
    const auto split = units.at("split_kbps").get<std::int64_t>();
    auto bounded = requested * units.at("warp_factor").get<std::int64_t>();
    if (!units.at("cap_kbps").is_null()) {
      bounded = std::min(bounded, units.at("cap_kbps").get<std::int64_t>());
    }
    EXPECT_EQ(split, bounded);
    const auto encoder = units.at("encoder_kbps").get<std::int64_t>();
    const auto fec = units.at("fec_percentage").get<int>();
    const auto audio = units.at("audio_kbps").get<int>();
    EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(split, fec, audio), encoder);
    const auto smallest = stream_bitrate::wire_kbps_for_encoder(encoder, fec, audio);
    EXPECT_LE(smallest, split);
    EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(smallest, fec, audio), encoder);
    EXPECT_LT(stream_bitrate::encoder_kbps_for_wire(smallest - 1, fec, audio), encoder);
  }
}  // namespace

TEST(StreamStatsBitrateUnitsTests, EveryCodecsSessionStatusCarriesItsBitrateUnits) {
  // pyrowave_bitrate is there only for PyroWave. bitrate_units is there for every stream.
  stream_stats::update_stream_active(false);
  const std::string ip = "203.0.113.47";
  constexpr std::uint64_t generation = 471;
  for (const std::string codec : {"h264", "hevc", "av1", "pyrowave"}) {
    SCOPED_TRACE(codec);
    stream_stats::add_client(ip, "BitrateUnits", generation);
    const auto cleanup = util::fail_guard([&] {
      stream_stats::remove_client(ip, generation);
      stream_stats::update_stream_active(false);
    });
    const auto request = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
    stream_stats::update_video_stats(ip, 0, request.encoder_kbps, 0, codec, 1920, 1080, {}, generation);
    // Nothing until the handshake is recorded: its zeros would read as figures.
    EXPECT_TRUE(stream_stats::bitrate_units_json(stream_stats::get_current(), generation).is_null());
    EXPECT_FALSE(stream_stats::recorded_stream_request(generation).has_value());
    ASSERT_TRUE(stream_stats::record_stream_request(generation, false, request));

    const auto stats = stream_stats::get_current();
    EXPECT_EQ(stats.codec, codec);
    const auto units = stream_stats::bitrate_units_json(stats, generation);
    EXPECT_EQ(units, (nlohmann::json {
      {"version", 1},
      {"requested_kbps", 50000},
      {"warp_factor", 1},
      {"cap_kbps", nullptr},
      {"cap_source", nullptr},
      {"split_kbps", 50000},
      {"encoder_kbps", 43987},
      {"live_encoder_kbps", 43987},
      {"audio_kbps", 512},
      {"fec_percentage", 10},
      {"formula", "stream_bitrate_v1"},
    }));
    expect_round_trip(units);
    // A client with no stream here is answered about this one.
    EXPECT_EQ(stream_stats::bitrate_units_json(stats, 0), units);
    ASSERT_TRUE(stream_stats::recorded_stream_request(generation).has_value());
    EXPECT_EQ(*stream_stats::recorded_stream_request(generation), request);
  }
  EXPECT_TRUE(stream_stats::bitrate_units_json(stream_stats::get_current(), generation).is_null());
  EXPECT_FALSE(stream_stats::recorded_stream_request(generation).has_value());
  EXPECT_FALSE(stream_stats::recorded_stream_request(0).has_value());
}

TEST(StreamStatsBitrateUnitsTests, TheFiguresRoundTripThroughStreamBitrate) {
  for (const std::int64_t total : {1000, 2500, 5000, 20000, 50000, 150000, 300000, 500000}) {
    for (const int fec : {0, 10, 20, 50, 80, 90}) {
      for (const int audio : {192, 512, 576, 1536, 2048}) {
        SCOPED_TRACE(std::to_string(total) + " kbps at " + std::to_string(fec) + "% FEC with " +
                     std::to_string(audio) + " kbps audio");
        const auto request = negotiated_request(total, fec, audio);
        const auto units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(7, request)}), 7);
        EXPECT_EQ(units.at("requested_kbps"), total);
        EXPECT_EQ(units.at("split_kbps"), total);
        EXPECT_EQ(units.at("fec_percentage"), fec);
        EXPECT_EQ(units.at("audio_kbps"), audio);
        EXPECT_EQ(units.at("encoder_kbps"), stream_bitrate::encoder_kbps_for_wire(total, fec, audio));
        expect_round_trip(units);
      }
    }
  }
}

TEST(StreamStatsBitrateUnitsTests, SurroundAudioComesOffTheRequestAndIsReportedAsItsOwn) {
  // 80 Mbps at 10% FEC. The audio is what the handshake counts for the stream's channels and quality.
  const auto units_for = [](bool high_quality, int channels) {
    const auto request = negotiated_request(80000, 10, stream_bitrate::audio_kbps(high_quality, channels));
    return stream_stats::bitrate_units_json(streaming_stats({recorded_stream(9, request)}), 9);
  };
  const auto stereo_low = units_for(false, 2);
  const auto stereo = units_for(true, 2);
  const auto surround51 = units_for(true, 6);
  const auto surround71 = units_for(true, 8);

  EXPECT_EQ(stereo_low.at("audio_kbps"), 192);
  EXPECT_EQ(stereo.at("audio_kbps"), 512);
  EXPECT_EQ(surround51.at("audio_kbps"), 1536);
  EXPECT_EQ(surround71.at("audio_kbps"), 2048);
  EXPECT_EQ(stereo_low.at("encoder_kbps"), 71308);
  EXPECT_EQ(stereo.at("encoder_kbps"), 70988);
  EXPECT_EQ(surround51.at("encoder_kbps"), 69964);
  EXPECT_EQ(surround71.at("encoder_kbps"), 69452);
  // The same request, and the difference in audio is exactly what the encoder gives up.
  EXPECT_EQ(stereo.at("encoder_kbps").get<int>() - surround51.at("encoder_kbps").get<int>(), 1536 - 512);
  for (const auto &units : {stereo_low, stereo, surround51, surround71}) {
    EXPECT_EQ(units.at("requested_kbps"), 80000);
    EXPECT_EQ(units.at("fec_percentage"), 10);
    expect_round_trip(units);
  }
  // So the same encoder rate asks more of a surround stream than of a stereo one.
  EXPECT_GT(stream_bitrate::wire_kbps_for_encoder(70988, 10, 1536), 80000);
}

TEST(StreamStatsBitrateUnitsTests, TheFecShareChangesTheSplitUntilTheHandshakeStopsTakingItOff) {
  const auto units_for = [](int fec) {
    const auto request = negotiated_request(80000, fec, stream_bitrate::audio_kbps(true, 2));
    return stream_stats::bitrate_units_json(streaming_stats({recorded_stream(11, request)}), 11);
  };
  const auto none = units_for(0);
  const auto ten = units_for(10);
  const auto twenty = units_for(20);
  const auto half = units_for(50);
  const auto most = units_for(80);
  const auto beyond = units_for(90);

  EXPECT_EQ(none.at("encoder_kbps"), 78988);
  EXPECT_EQ(ten.at("encoder_kbps"), 70988);
  EXPECT_EQ(twenty.at("encoder_kbps"), 62988);
  EXPECT_EQ(half.at("encoder_kbps"), 38988);
  EXPECT_EQ(most.at("encoder_kbps"), 14988);
  // Above 80% the handshake takes no FEC off, so the split is the one with none.
  EXPECT_EQ(beyond.at("encoder_kbps"), none.at("encoder_kbps"));
  for (const auto &[fec, units] : std::vector<std::pair<int, nlohmann::json>> {
         {0, none}, {10, ten}, {20, twenty}, {50, half}, {80, most}, {90, beyond}}) {
    SCOPED_TRACE(fec);
    EXPECT_EQ(units.at("fec_percentage"), fec);
    EXPECT_EQ(units.at("requested_kbps"), 80000);
    EXPECT_EQ(units.at("audio_kbps"), 512);
    expect_round_trip(units);
  }
}

TEST(StreamStatsBitrateUnitsTests, TheLiveEncoderRateFollowsTheEncodeLoopAndTheNegotiatedOneStays) {
  stream_stats::update_stream_active(false);
  const std::string ip = "203.0.113.48";
  constexpr std::uint64_t generation = 472;
  stream_stats::add_client(ip, "BitrateUnitsLive", generation);
  const auto cleanup = util::fail_guard([&] {
    stream_stats::remove_client(ip, generation);
    stream_stats::update_stream_active(false);
  });
  const auto request = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  stream_stats::update_video_stats(ip, 0, request.encoder_kbps, 0, "hevc", 2560, 1440, {}, generation);
  ASSERT_TRUE(stream_stats::record_stream_request(generation, false, request));

  // The session's own encode loop reports the rate it applied after Live Tuning or a live bitrate cut it.
  stream_stats::update_video_stats(60.0, 30000, 2.0, "hevc", 2560, 1440, "nvenc", generation);
  auto units = stream_stats::bitrate_units_json(stream_stats::get_current(), generation);
  EXPECT_EQ(units.at("live_encoder_kbps"), 30000);
  EXPECT_EQ(units.at("encoder_kbps"), 43987);
  EXPECT_EQ(units.at("requested_kbps"), 50000);
  expect_round_trip(units);

  // And the rate it climbed back to.
  stream_stats::update_video_stats(60.0, 43987, 2.0, "hevc", 2560, 1440, "nvenc", generation);
  units = stream_stats::bitrate_units_json(stream_stats::get_current(), generation);
  EXPECT_EQ(units.at("live_encoder_kbps"), 43987);
}

TEST(StreamStatsBitrateUnitsTests, AStreamWithNoSplitOfItsOwnSaysSoWithANullSplit) {
  // A watcher: its own request was split, then the handshake put its encoder on its owner's rate. The
  // formula makes 43987 of its 50000, not 20000, so the split is null and a client knows not to use it.
  auto watcher = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  EXPECT_EQ(stream_bitrate::settle_encoder(watcher, watcher.encoder_kbps, 20000), 20000);
  const auto units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(13, watcher)}), 13);
  EXPECT_EQ(units.at("requested_kbps"), 50000);
  EXPECT_TRUE(units.at("split_kbps").is_null()) << units.dump();
  EXPECT_EQ(units.at("encoder_kbps"), 20000);
  EXPECT_EQ(units.at("live_encoder_kbps"), 20000);
  EXPECT_EQ(units.at("audio_kbps"), 512);
  EXPECT_EQ(units.at("fec_percentage"), 10);
  EXPECT_NE(stream_bitrate::encoder_kbps_for_wire(50000, 10, 512), 20000);

  // A client that sent no bitrate at all: nothing to split, but its audio and FEC are still recorded.
  const auto unsplit = negotiated_request(0, 10, stream_bitrate::audio_kbps(true, 6));
  const auto none = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(14, unsplit)}), 14);
  EXPECT_EQ(none.at("requested_kbps"), 0);
  EXPECT_TRUE(none.at("split_kbps").is_null()) << none.dump();
  EXPECT_EQ(none.at("encoder_kbps"), 0);
  EXPECT_EQ(none.at("audio_kbps"), 1536);
  EXPECT_EQ(none.at("fec_percentage"), 10);
}

TEST(StreamStatsBitrateUnitsTests, TheRequestIsTheClientsOwnAndTheSplitIsWhatTheHostLeftOfIt) {
  // A Stability preset caps an HEVC request of 150 Mbps at 40: the client asked for 150, and 40 was
  // split. The cap says so.
  stream_bitrate::request_t capped;
  capped.client_kbps = 150000;
  capped.cap_kbps = 40000;
  capped.cap_source = "stability_preset_selected";
  const auto capped_encoder = stream_bitrate::split_request(capped, 40000, 10, 512);
  ASSERT_TRUE(capped_encoder.has_value());
  stream_bitrate::settle_encoder(capped, static_cast<int>(*capped_encoder), std::nullopt);
  const auto capped_units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(21, capped)}), 21);
  EXPECT_EQ(capped_units.at("requested_kbps"), 150000);
  EXPECT_EQ(capped_units.at("warp_factor"), 1);
  EXPECT_EQ(capped_units.at("cap_kbps"), 40000);
  EXPECT_EQ(capped_units.at("cap_source"), "stability_preset_selected");
  EXPECT_EQ(capped_units.at("split_kbps"), 40000);
  EXPECT_EQ(capped_units.at("encoder_kbps"), 34988);
  expect_round_trip(capped_units);

  // limit_framerate warps a request of 20 Mbps to 40 with nothing to cap it: the client asked for 20.
  stream_bitrate::request_t warped;
  warped.client_kbps = 20000;
  warped.warp_factor = 2;
  const auto warped_encoder = stream_bitrate::split_request(warped, 40000, 10, 512);
  ASSERT_TRUE(warped_encoder.has_value());
  stream_bitrate::settle_encoder(warped, static_cast<int>(*warped_encoder), std::nullopt);
  const auto warped_units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(22, warped)}), 22);
  EXPECT_EQ(warped_units.at("requested_kbps"), 20000);
  EXPECT_EQ(warped_units.at("warp_factor"), 2);
  EXPECT_TRUE(warped_units.at("cap_kbps").is_null());
  EXPECT_TRUE(warped_units.at("cap_source").is_null());
  EXPECT_EQ(warped_units.at("split_kbps"), 40000);
  EXPECT_EQ(warped_units.at("encoder_kbps"), 34988);
  expect_round_trip(warped_units);
}

TEST(StreamStatsBitrateUnitsTests, EachClientIsAnsweredAboutItsOwnStream) {
  const auto stereo = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  const auto surround = negotiated_request(80000, 10, stream_bitrate::audio_kbps(true, 6));
  stream_stats::client_stats_t starting;
  starting.session_generation = 3;
  const auto stats = streaming_stats({recorded_stream(1, stereo), recorded_stream(2, surround), starting});

  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 2).at("requested_kbps"), 80000);
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 2).at("audio_kbps"), 1536);
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 1).at("requested_kbps"), 50000);
  // No stream here: the first one that has recorded its handshake.
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 0).at("requested_kbps"), 50000);
  // A client whose own stream is not in stats gets nothing rather than another stream's figures: its
  // stream was torn down a moment before its session timing stopped, or a reconnect replaced it.
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 99).is_null());
  // A stream still in its handshake gets nothing rather than another stream's figures.
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 3).is_null());

  auto idle = stats;
  idle.streaming = false;
  EXPECT_TRUE(stream_stats::bitrate_units_json(idle, 1).is_null());
  EXPECT_TRUE(stream_stats::bitrate_units_json(streaming_stats({starting}), 0).is_null());
}

