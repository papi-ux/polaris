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
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
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


namespace {
  // The admin stream stats as GET /api/stats/stream and its SSE twin serialize them.
  nlohmann::json admin_stats_json(const stream_stats::stats_t &stats) {
    return nlohmann::json::parse(stats.to_json());
  }

  // The one admin row for a stream, found by its opaque identity, the way a reader finds its own.
  nlohmann::json admin_row(const nlohmann::json &admin, std::uint64_t generation) {
    const auto id = stream_stats::stream_instance_id(generation);
    nlohmann::json found;
    int matches = 0;
    for (const auto &row : admin.at("clients")) {
      if (row.value("stream_instance_id", std::string {}) == id) {
        found = row;
        ++matches;
      }
    }
    EXPECT_EQ(matches, 1) << id << " in " << admin.dump();
    return found;
  }

  bool contains_key_anywhere(const nlohmann::json &value, const std::string &key) {
    if (value.is_object()) {
      for (const auto &[name, child] : value.items()) {
        if (name == key || contains_key_anywhere(child, key)) {
          return true;
        }
      }
    } else if (value.is_array()) {
      for (const auto &child : value) {
        if (contains_key_anywhere(child, key)) {
          return true;
        }
      }
    }
    return false;
  }

  bool contains_number_anywhere(const nlohmann::json &value, std::uint64_t number) {
    if (value.is_number_unsigned()) {
      return value.get<std::uint64_t>() == number;
    }
    if (value.is_number_integer()) {
      return value.get<std::int64_t>() == static_cast<std::int64_t>(number);
    }
    if (value.is_structured()) {
      for (const auto &child : value) {
        if (contains_number_anywhere(child, number)) {
          return true;
        }
      }
    }
    return false;
  }

  std::set<std::string> keys_of(const nlohmann::json &object) {
    std::set<std::string> keys;
    for (const auto &[name, value] : object.items()) {
      keys.insert(name);
    }
    return keys;
  }

  // Neither name for the object at the top level, with any number of rows: a top-level copy would be
  // some one stream's figures answering for the whole host.
  void expect_no_top_level_copy(const nlohmann::json &admin) {
    EXPECT_FALSE(admin.contains("handshake_bitrate_units")) << admin.dump();
    EXPECT_FALSE(admin.contains("bitrate_units")) << admin.dump();
  }

  // The row's handshake_bitrate_units is its own stream's record, exactly what the paired session
  // status answers that stream's own client, under the row's own identity and with no authority.
  void expect_own_handshake(const stream_stats::stats_t &stats, const nlohmann::json &admin, std::uint64_t generation) {
    SCOPED_TRACE("generation " + std::to_string(generation));
    const auto row = admin_row(admin, generation);
    ASSERT_TRUE(row.contains("handshake_bitrate_units")) << row.dump();
    const auto &handshake = row.at("handshake_bitrate_units");
    const auto paired = stream_stats::bitrate_units_json(stats, generation);
    ASSERT_TRUE(paired.is_object()) << "the stream has recorded no handshake";
    EXPECT_EQ(handshake.at("stream_instance_id"), row.at("stream_instance_id"));
    EXPECT_EQ(handshake.at("available"), true);
    EXPECT_EQ(handshake.at("reason"), "recorded");
    EXPECT_EQ(handshake.at("action_authority"), false);
    EXPECT_EQ(handshake.at("units"), paired);
  }

  // The row says it has no record of its own, with null units, rather than leaving the key out or
  // carrying another stream's figures.
  void expect_unavailable_handshake(const nlohmann::json &admin, std::uint64_t generation, const std::string &reason) {
    SCOPED_TRACE("generation " + std::to_string(generation));
    const auto row = admin_row(admin, generation);
    ASSERT_TRUE(row.contains("handshake_bitrate_units")) << row.dump();
    const auto &handshake = row.at("handshake_bitrate_units");
    EXPECT_EQ(handshake.at("stream_instance_id"), row.at("stream_instance_id"));
    EXPECT_EQ(handshake.at("available"), false);
    EXPECT_EQ(handshake.at("reason"), reason);
    EXPECT_EQ(handshake.at("action_authority"), false);
    EXPECT_TRUE(handshake.at("units").is_null()) << handshake.dump();
  }
}  // namespace

TEST(StreamStatsBitrateUnitsTests, AnAdminRowCarriesItsOwnHandshakeAsAnAsOfDiagnostic) {
  stream_stats::update_stream_active(false);
  const auto reset = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  const std::string ip = "203.0.113.61";
  constexpr std::uint64_t generation = 481;
  stream_stats::add_client(ip, "HandshakeUnits", generation);
  const auto request = negotiated_request(30000, 10, stream_bitrate::audio_kbps(true, 2));
  stream_stats::update_video_stats(ip, 0, request.encoder_kbps, 0, "hevc", 1920, 1080, {}, generation);

  // Still in its handshake: the row says so rather than leaving the key out or filling in zeros.
  auto admin = admin_stats_json(stream_stats::get_current());
  expect_unavailable_handshake(admin, generation, "handshake_not_recorded");
  expect_no_top_level_copy(admin);

  ASSERT_TRUE(stream_stats::record_stream_request(generation, false, request));
  const auto stats = stream_stats::get_current();
  admin = admin_stats_json(stats);
  expect_own_handshake(stats, admin, generation);
  const auto handshake = admin_row(admin, generation).at("handshake_bitrate_units");
  EXPECT_EQ(handshake, (nlohmann::json {
    {"version", 1},
    {"stream_instance_id", stream_stats::stream_instance_id(generation)},
    {"available", true},
    {"reason", "recorded"},
    {"basis", "rtsp_handshake_record"},
    {"snapshot", "as_of_diagnostic"},
    {"action_authority", false},
    {"units", stream_stats::bitrate_units_json(stats, generation)},
  }));
  // Many different splits land an encoder on 25987. The record says which one this was: the client
  // asked for 30000, nothing warped or capped it, and 10% FEC and stereo audio came off.
  const auto &units = handshake.at("units");
  EXPECT_EQ(units.at("requested_kbps"), 30000);
  EXPECT_EQ(units.at("warp_factor"), 1);
  EXPECT_TRUE(units.at("cap_kbps").is_null());
  EXPECT_EQ(units.at("split_kbps"), 30000);
  EXPECT_EQ(units.at("encoder_kbps"), 25987);
  EXPECT_EQ(units.at("live_encoder_kbps"), 25987);
  EXPECT_EQ(units.at("audio_kbps"), 512);
  EXPECT_EQ(units.at("fec_percentage"), 10);
  expect_round_trip(units);
  // One row, and still no top-level copy: the single-client mirrors stop at capture and encoder.
  ASSERT_EQ(admin.at("clients").size(), 1u);
  expect_no_top_level_copy(admin);
}

TEST(StreamStatsBitrateUnitsTests, EachAdminRowIsItsOwnNotTheFirstRecordedNorTheLastRecorder) {
  stream_stats::update_stream_active(false);
  const auto reset = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  constexpr std::uint64_t first = 482;
  constexpr std::uint64_t second = 483;
  stream_stats::add_client("203.0.113.62", "First", first);
  stream_stats::add_client("203.0.113.63", "Second", second);
  const auto stereo = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  const auto surround = negotiated_request(80000, 10, stream_bitrate::audio_kbps(true, 6));

  ASSERT_TRUE(stream_stats::record_stream_request(first, false, stereo));
  auto stats = stream_stats::get_current();
  auto admin = admin_stats_json(stats);
  expect_own_handshake(stats, admin, first);
  // A paired client with no stream here would be told about the first row. The second row, in its
  // handshake, is not.
  ASSERT_TRUE(stream_stats::bitrate_units_json(stats, 0).is_object());
  expect_unavailable_handshake(admin, second, "handshake_not_recorded");

  ASSERT_TRUE(stream_stats::record_stream_request(second, false, surround));
  stats = stream_stats::get_current();
  admin = admin_stats_json(stats);
  // The host's top-level request follows the last recorder, the second, and a paired client with no
  // stream is still answered about the first. Neither decides a row.
  ASSERT_EQ(stats.bitrate_request, surround);
  ASSERT_EQ(stream_stats::bitrate_units_json(stats, 0), stream_stats::bitrate_units_json(stats, first));
  expect_own_handshake(stats, admin, first);
  expect_own_handshake(stats, admin, second);
  const auto first_units = admin_row(admin, first).at("handshake_bitrate_units").at("units");
  const auto second_units = admin_row(admin, second).at("handshake_bitrate_units").at("units");
  EXPECT_NE(first_units, second_units);
  EXPECT_EQ(first_units.at("requested_kbps"), 50000);
  EXPECT_EQ(first_units.at("audio_kbps"), 512);
  EXPECT_EQ(second_units.at("requested_kbps"), 80000);
  EXPECT_EQ(second_units.at("audio_kbps"), 1536);
  expect_round_trip(first_units);
  expect_round_trip(second_units);
  expect_no_top_level_copy(admin);
}

TEST(StreamStatsBitrateUnitsTests, AWatchersAdminRowKeepsItsOwnRequestAndItsNullSplit) {
  stream_stats::update_stream_active(false);
  const auto reset = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  constexpr std::uint64_t owner = 491;
  constexpr std::uint64_t watcher = 492;
  stream_stats::add_client("203.0.113.64", "Owner", owner);
  stream_stats::add_client("203.0.113.65", "Watcher", watcher);
  const auto owned = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  // The watcher's own request was split, then the handshake put its encoder on its owner's rate.
  auto watching = negotiated_request(30000, 10, stream_bitrate::audio_kbps(true, 6));
  ASSERT_EQ(stream_bitrate::settle_encoder(watching, watching.encoder_kbps, owned.encoder_kbps), owned.encoder_kbps);
  ASSERT_TRUE(stream_stats::record_stream_request(owner, false, owned));
  ASSERT_TRUE(stream_stats::record_stream_request(watcher, false, watching));

  const auto stats = stream_stats::get_current();
  const auto admin = admin_stats_json(stats);
  expect_own_handshake(stats, admin, owner);
  expect_own_handshake(stats, admin, watcher);
  const auto owner_units = admin_row(admin, owner).at("handshake_bitrate_units").at("units");
  const auto watcher_units = admin_row(admin, watcher).at("handshake_bitrate_units").at("units");
  expect_round_trip(owner_units);
  // The formula says nothing about the watcher, and its row does not borrow the owner's split.
  EXPECT_TRUE(watcher_units.at("split_kbps").is_null()) << watcher_units.dump();
  EXPECT_EQ(owner_units.at("split_kbps"), 50000);
  EXPECT_EQ(watcher_units.at("encoder_kbps"), owned.encoder_kbps);
  EXPECT_EQ(watcher_units.at("requested_kbps"), 30000);
  EXPECT_EQ(watcher_units.at("audio_kbps"), 1536);
  EXPECT_EQ(owner_units.at("requested_kbps"), 50000);
  EXPECT_EQ(owner_units.at("audio_kbps"), 512);
}

TEST(StreamStatsBitrateUnitsTests, AReconnectsAdminRowsNeverFillOneFromTheOther) {
  stream_stats::update_stream_active(false);
  const auto reset = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  const std::string ip = "203.0.113.66";
  constexpr std::uint64_t replaced = 511;
  constexpr std::uint64_t reconnect = 512;
  const auto stereo = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  const auto surround = negotiated_request(80000, 10, stream_bitrate::audio_kbps(true, 6));
  stream_stats::add_client(ip, "Device", replaced);
  ASSERT_TRUE(stream_stats::record_stream_request(replaced, false, stereo));

  // The same device reconnects while the stream it replaces is still listed.
  stream_stats::add_client(ip, "Device", reconnect);
  auto stats = stream_stats::get_current();
  auto admin = admin_stats_json(stats);
  expect_own_handshake(stats, admin, replaced);
  expect_unavailable_handshake(admin, reconnect, "handshake_not_recorded");
  expect_no_top_level_copy(admin);

  ASSERT_TRUE(stream_stats::record_stream_request(reconnect, false, surround));
  stats = stream_stats::get_current();
  admin = admin_stats_json(stats);
  expect_own_handshake(stats, admin, replaced);
  expect_own_handshake(stats, admin, reconnect);
  EXPECT_EQ(admin_row(admin, replaced).at("handshake_bitrate_units").at("units").at("audio_kbps"), 512);
  EXPECT_EQ(admin_row(admin, reconnect).at("handshake_bitrate_units").at("units").at("audio_kbps"), 1536);
  expect_no_top_level_copy(admin);

  // The replaced stream goes, and the reconnect alone is listed, with its own record and still no
  // top-level copy now that one row could be mirrored.
  stream_stats::remove_client(ip, replaced);
  stats = stream_stats::get_current();
  admin = admin_stats_json(stats);
  ASSERT_EQ(admin.at("clients").size(), 1u);
  expect_own_handshake(stats, admin, reconnect);
  EXPECT_EQ(admin_row(admin, reconnect).at("handshake_bitrate_units").at("units").at("requested_kbps"), 80000);
  expect_no_top_level_copy(admin);
}

TEST(StreamStatsBitrateUnitsTests, NothingStreamingMeansNoRecordedHandshakeIsServed) {
  stream_stats::update_stream_active(false);
  const auto reset = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  const std::string ip = "203.0.113.67";
  constexpr std::uint64_t generation = 521;
  const auto request = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  stream_stats::add_client(ip, "Idle", generation);
  ASSERT_TRUE(stream_stats::record_stream_request(generation, false, request));
  ASSERT_TRUE(admin_row(admin_stats_json(stream_stats::get_current()), generation)
                .at("handshake_bitrate_units").at("available").get<bool>());

  // The stream ends: nothing is listed, the ended session keeps no copy, and nothing is left anywhere.
  stream_stats::remove_client(ip, generation);
  auto admin = admin_stats_json(stream_stats::get_current());
  EXPECT_EQ(admin.at("streaming"), false);
  EXPECT_TRUE(admin.at("clients").empty());
  EXPECT_FALSE(contains_key_anywhere(admin, "handshake_bitrate_units")) << admin.dump();
  stream_stats::update_stream_active(false);
  admin = admin_stats_json(stream_stats::get_current());
  EXPECT_FALSE(contains_key_anywhere(admin, "handshake_bitrate_units")) << admin.dump();

  // A snapshot that still lists a recorded row while nothing streams says not_streaming, with null units.
  auto idle = streaming_stats({recorded_stream(generation, request)});
  idle.streaming = false;
  expect_unavailable_handshake(admin_stats_json(idle), generation, "not_streaming");
  EXPECT_TRUE(stream_stats::bitrate_units_json(idle, generation).is_null());
}

TEST(StreamStatsBitrateUnitsTests, ARowWithoutItsOwnRecordOrGenerationGetsNoOtherStreamsFigures) {
  const auto stereo = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  // In its handshake, listed ahead of the one stream that has recorded.
  stream_stats::client_stats_t starting;
  starting.session_generation = 3;
  // A legacy row with no generation, carrying a record anyway: it has no stream identity to name.
  auto legacy = recorded_stream(0, stereo);
  legacy.name = "Legacy";
  legacy.ip = "203.0.113.68";
  auto stats = streaming_stats({starting, recorded_stream(1, stereo), legacy});
  // The host's single stream identity names another stream than the starting row.
  stats.session_generation = 1;
  const auto admin = admin_stats_json(stats);

  expect_unavailable_handshake(admin, 3, "handshake_not_recorded");
  expect_own_handshake(stats, admin, 1);
  // The paired selection is unchanged: no stream here is the first recorded, a stream not here nothing.
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 0), stream_stats::bitrate_units_json(stats, 1));
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 99).is_null());
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 3).is_null());

  const auto &rows = admin.at("clients");
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_FALSE(rows[2].contains("stream_instance_id")) << rows[2].dump();
  EXPECT_FALSE(rows[2].contains("handshake_bitrate_units")) << rows[2].dump();
  // Alone, the legacy row still gets none, though its record is the only one listed.
  const auto alone = admin_stats_json(streaming_stats({legacy}));
  ASSERT_EQ(alone.at("clients").size(), 1u);
  EXPECT_FALSE(contains_key_anywhere(alone, "handshake_bitrate_units")) << alone.dump();
  expect_no_top_level_copy(admin);
}

TEST(StreamStatsBitrateUnitsTests, TheRowsObjectIsThePairedObjectForEveryShapeOfRequest) {
  // The paired session status and the admin row answer the same, so a capped, warped, watching or
  // unsplit stream reads the same in both, and the paired selection still picks it.
  stream_bitrate::request_t capped;
  capped.client_kbps = 150000;
  capped.cap_kbps = 40000;
  capped.cap_source = "stability_preset_selected";
  ASSERT_TRUE(stream_bitrate::split_request(capped, 40000, 10, 512).has_value());
  stream_bitrate::settle_encoder(capped, capped.encoder_kbps, std::nullopt);
  stream_bitrate::request_t warped;
  warped.client_kbps = 20000;
  warped.warp_factor = 2;
  ASSERT_TRUE(stream_bitrate::split_request(warped, 40000, 10, 512).has_value());
  stream_bitrate::settle_encoder(warped, warped.encoder_kbps, std::nullopt);
  auto watching = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  stream_bitrate::settle_encoder(watching, watching.encoder_kbps, 20000);
  const auto unsplit = negotiated_request(0, 10, stream_bitrate::audio_kbps(true, 6));
  const auto plain = negotiated_request(60000, 20, stream_bitrate::audio_kbps(true, 8));

  std::uint64_t generation = 600;
  for (const auto &request : {capped, warped, watching, unsplit, plain}) {
    ++generation;
    SCOPED_TRACE(generation);
    const auto client = recorded_stream(generation, request);
    const auto stats = streaming_stats({client});
    const auto paired = stream_stats::bitrate_units_json(stats, generation);
    ASSERT_TRUE(paired.is_object());
    EXPECT_EQ(stream_stats::bitrate_units_json(stats, 0), paired);
    expect_own_handshake(stats, admin_stats_json(stats), generation);
  }
  // And a lone row in its handshake is unavailable in both.
  stream_stats::client_stats_t starting;
  starting.session_generation = 610;
  const auto stats = streaming_stats({starting});
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 610).is_null());
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 0).is_null());
  expect_unavailable_handshake(admin_stats_json(stats), 610, "handshake_not_recorded");
}

TEST(StreamStatsBitrateUnitsTests, TheAdminHandshakeNamesNoClientAndNoPrivateIdentity) {
  stream_bitrate::request_t capped;
  capped.client_kbps = 150000;
  capped.cap_kbps = 40000;
  capped.cap_source = "stability_preset_selected";
  ASSERT_TRUE(stream_bitrate::split_request(capped, 40000, 10, 512).has_value());
  stream_bitrate::settle_encoder(capped, capped.encoder_kbps, std::nullopt);
  // A generation no figure here could equal, so a number that matches it can only be the generation.
  constexpr std::uint64_t recorded_generation = 48163;
  constexpr std::uint64_t starting_generation = 48167;
  auto recorded = recorded_stream(recorded_generation, capped);
  recorded.name = "Living Room Deck";
  recorded.ip = "198.51.100.23";
  recorded.client_family = "nova";
  recorded.started_at = std::chrono::system_clock::now();
  stream_stats::client_stats_t starting;
  starting.session_generation = starting_generation;
  starting.name = "Bedroom Phone";
  starting.ip = "198.51.100.24";
  auto stats = streaming_stats({recorded, starting});
  stats.session_generation = recorded_generation;
  const auto admin = admin_stats_json(stats);

  const std::set<std::string> wrapper_keys {
    "version", "stream_instance_id", "available", "reason", "basis", "snapshot", "action_authority", "units"};
  for (const auto &[generation, client] : std::vector<std::pair<std::uint64_t, stream_stats::client_stats_t>> {
         {recorded_generation, recorded}, {starting_generation, starting}}) {
    SCOPED_TRACE(generation);
    const auto handshake = admin_row(admin, generation).at("handshake_bitrate_units");
    EXPECT_EQ(keys_of(handshake), wrapper_keys);
    for (const auto *key : {"session_generation", "generation", "app_session_id", "session_id", "session_token",
                            "uuid", "device_uuid", "ip", "client_ip", "name", "client_name", "client_family",
                            "started_at", "ended_at", "received_at", "timestamp"}) {
      EXPECT_FALSE(contains_key_anywhere(handshake, key)) << key << " in " << handshake.dump();
    }
    EXPECT_FALSE(contains_number_anywhere(handshake, generation)) << handshake.dump();
    const auto text = handshake.dump();
    EXPECT_EQ(text.find(client.name), std::string::npos) << text;
    EXPECT_EQ(text.find(client.ip), std::string::npos) << text;
    // The stream is named only by its opaque identity, the same one its row carries.
    EXPECT_EQ(handshake.at("stream_instance_id"), stream_stats::stream_instance_id(generation));
  }
  // The units carry exactly the paired object's fields, and nothing more about the client.
  const auto units = admin_row(admin, recorded_generation).at("handshake_bitrate_units").at("units");
  EXPECT_EQ(keys_of(units), keys_of(stream_stats::bitrate_units_json(stats, recorded_generation)));
  EXPECT_EQ(units.at("cap_source"), "stability_preset_selected");
}

TEST(StreamStatsBitrateUnitsTests, TheAdminHandshakeGrantsNothingAndChangesNothingElseServed) {
  const auto stereo = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  const auto surround = negotiated_request(80000, 10, stream_bitrate::audio_kbps(true, 6));
  const auto stats = streaming_stats({recorded_stream(21, stereo), recorded_stream(22, surround)});
  const auto served = admin_stats_json(stats);
  const auto without_handshakes = [](nlohmann::json admin) {
    for (auto &row : admin.at("clients")) {
      row.erase("handshake_bitrate_units");
    }
    return admin;
  };

  // Another record on one row changes that row's handshake_bitrate_units and nothing else served: no
  // other row, no top-level figure, no Doctor finding or action scope, no Live Tuning field.
  auto changed = stats;
  changed.clients[0].bitrate_request = negotiated_request(60000, 20, stream_bitrate::audio_kbps(true, 8));
  const auto changed_served = admin_stats_json(changed);
  EXPECT_NE(changed_served.at("clients")[0].at("handshake_bitrate_units"),
            served.at("clients")[0].at("handshake_bitrate_units"));
  EXPECT_EQ(changed_served.at("clients")[1].at("handshake_bitrate_units"),
            served.at("clients")[1].at("handshake_bitrate_units"));
  EXPECT_EQ(without_handshakes(changed_served), without_handshakes(served));

  // So does a row losing its record.
  auto unrecorded = stats;
  unrecorded.clients[1].bitrate_request = {};
  unrecorded.clients[1].bitrate_request_recorded = false;
  const auto unrecorded_served = admin_stats_json(unrecorded);
  expect_unavailable_handshake(unrecorded_served, 22, "handshake_not_recorded");
  EXPECT_EQ(without_handshakes(unrecorded_served), without_handshakes(served));

  // Nothing in it is something to act on: no authority, no action, payload or revision to apply.
  for (const auto &row : served.at("clients")) {
    const auto &handshake = row.at("handshake_bitrate_units");
    EXPECT_EQ(handshake.at("action_authority"), false);
    EXPECT_EQ(handshake.at("basis"), "rtsp_handshake_record");
    EXPECT_EQ(handshake.at("snapshot"), "as_of_diagnostic");
    for (const auto *key : {"action", "action_id", "actions", "payload_preview", "expected_revision",
                            "expected_configuration_revision", "configuration_revision", "applied_revision"}) {
      EXPECT_FALSE(contains_key_anywhere(handshake, key)) << key << " in " << handshake.dump();
    }
  }
}
