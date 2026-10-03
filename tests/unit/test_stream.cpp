/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <src/stream_start_outcome.h>
#include <src/stream_stats.h>
#include <src/utility.h>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
  void concat_and_insert_into(std::vector<uint8_t> &result, uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  bool control_packet_carries_type(std::size_t data_length);
  bool encrypted_control_header_present(std::size_t payload_size);
  bool encrypted_control_cipher_fits(std::size_t payload_size, std::uint16_t declared_length);
  bool input_control_cipher_fits(std::size_t payload_size, std::int32_t declared_cipher_length);
  void record_network_stats(const std::string &client_ip, double latency_ms, double packet_loss, std::uint64_t bytes_sent);
}

namespace nvhttp {
  std::string effective_session_encoder_name_for_tests(const stream_stats::stats_t &stats,
                                                       const std::string &launch_encoder);
  nlohmann::json build_session_health_json_for_tests(
    const stream_stats::stats_t &stats,
    bool current_virtual_display,
    const std::string &device_name,
    const std::string &app_name
  );
  nlohmann::json build_session_health_json_for_tests(
    const stream_stats::stats_t &stats,
    bool current_virtual_display,
    const std::string &device_name,
    const std::string &app_name,
    std::uint64_t requester_generation
  );
  std::uint64_t requester_session_generation_for_tests(const std::string &device_uuid);
  nlohmann::json session_encoder_identity_json_for_tests(const stream_stats::stats_t &stats,
                                                         const std::string &requested_backend,
                                                         const std::string &launch_backend,
                                                         bool session_override,
                                                         std::uint64_t requester_generation);
}

namespace confighttp {
  nlohmann::json stream_stats_json(const stream_stats::stats_t &stats);
}

namespace proc {
  bool should_publish_stream_ended_after_terminate_for_tests(bool had_running_app, int active_sessions, std::string_view session_state, bool cleanup_complete = true);

  nlohmann::json classify_host_pause_session_for_tests(
    const stream_stats::stats_t &stats,
    double target_fps,
    bool current_virtual_display
  );
}

namespace {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    std::vector<uint8_t> result;
    stream::concat_and_insert_into(result, insert_size, slice_size, data1, data2);
    return result;
  }

  stream_stats::stats_t stable_gpu_native_stats(double delivered_fps, double target_fps) {
    stream_stats::stats_t stats;
    stats.streaming = true;
    stats.runtime_effective_headless = true;
    stats.capture_transport = platf::frame_transport_e::dmabuf;
    stats.capture_residency = platf::frame_residency_e::gpu;
    stats.encode_target_residency = platf::frame_residency_e::gpu;
    stats.fps = delivered_fps;
    stats.encode_target_fps = target_fps;
    stats.capture_source_fps = target_fps;
    stats.codec = "hevc";
    return stats;
  }

  stream_stats::stats_t stable_cpu_copy_stats(double delivered_fps, double target_fps) {
    auto stats = stable_gpu_native_stats(delivered_fps, target_fps);
    stats.capture_transport = platf::frame_transport_e::shm;
    stats.capture_residency = platf::frame_residency_e::cpu;
    stats.capture_format = platf::frame_format_e::bgra8;
    stats.encode_target_device = "vaapi";
    stats.encode_target_residency = platf::frame_residency_e::gpu;
    stats.encode_target_format = platf::frame_format_e::nv12;
    return stats;
  }
}

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(NvhttpSessionHealthTests, HighRefreshNearTargetDeliveryRemainsSteady) {
  const auto health = nvhttp::build_session_health_json_for_tests(
    stable_gpu_native_stats(115.6, 120.0),
    false,
    "Nova Client",
    "Mouse"
  );

  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");
  EXPECT_FALSE(health.at("host_render_limited").get<bool>());
}

TEST(NvhttpSessionHealthTests, PyrowaveReportsItsActualEncoderAndCpuConversion) {
  auto stats = stable_cpu_copy_stats(120.0, 120.0);
  stats.codec = "pyrowave";
  stats.encode_target_device = "system";
  stats.encode_target_residency = platf::frame_residency_e::cpu;
  stats.encode_target_format = platf::frame_format_e::yuv420p;
  const auto health = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Test");
  EXPECT_EQ(health.at("active_encoder"), "pyrowave");
  const auto &selection = health.at("encoder_selection");
  EXPECT_EQ(selection.at("selected_encoder"), "pyrowave");
  EXPECT_EQ(selection.at("gpu_driver"), "unknown");
  EXPECT_FALSE(selection.at("fallback_used").get<bool>());
  EXPECT_TRUE(health.at("capture_cpu_copy").get<bool>());
  EXPECT_FALSE(health.at("capture_gpu_native").get<bool>());
}

TEST(NvhttpSessionHealthTests, PyrowaveReasonIsTheRouteItsOwnEncoderReported) {
  auto stats = stable_cpu_copy_stats(120.0, 120.0);
  stats.codec = "pyrowave";
  stats.clients.emplace_back();
  const auto reason_for = [&stats](std::string route) {
    stats.clients.front().pyrowave_route = std::move(route);
    const auto health = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Test");
    return health.at("encoder_selection").at("reason").get<std::string>();
  };

  // This is session_encoder_selection.reason, which Nova's Doctor card shows in two lines.
  const auto on_the_gpu = reason_for("gpu_upload");
  EXPECT_EQ(on_the_gpu.rfind("PyroWave converts colour on the GPU", 0), 0u) << on_the_gpu;
  EXPECT_EQ(on_the_gpu.find("CPU color conversion"), std::string::npos)
    << "a stream converting on the GPU is still said to convert on the CPU";
  const auto on_the_cpu = reason_for("cpu_convert");
  EXPECT_EQ(on_the_cpu.rfind("PyroWave converts colour on the CPU", 0), 0u) << on_the_cpu;
  EXPECT_EQ(reason_for("").rfind("PyroWave has not encoded a captured frame yet", 0), 0u);
}

/**
 * A paired client is answered about its own PyroWave stream.
 *
 * Watch Stream runs a session per watcher, each with its own encoder and route, and a reconnect keeps
 * the stream it replaced in the stats until that one's teardown. The reason used to give up on any
 * of those with a sentence that sent Nova to a field it never reads.
 */
TEST(NvhttpSessionHealthTests, PyrowaveReasonAnswersTheStreamThatAsked) {
  auto stats = stable_cpu_copy_stats(120.0, 120.0);
  stats.codec = "pyrowave";
  stats.clients.resize(2);
  stats.clients[0].session_generation = 391;
  stats.clients[0].pyrowave_route = "zero_copy";
  stats.clients[1].session_generation = 392;
  stats.clients[1].pyrowave_route = "cpu_convert";
  const auto reason_for = [&stats](std::uint64_t requester) {
    const auto health = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Test", requester);
    return health.at("encoder_selection").at("reason").get<std::string>();
  };

  const auto watcher = reason_for(392);
  EXPECT_EQ(watcher.rfind("PyroWave converts colour on the CPU", 0), 0u) << watcher;
  const auto owner = reason_for(391);
  EXPECT_EQ(owner.rfind("PyroWave imports captured DMA-BUF frames and converts colour on the GPU", 0), 0u) << owner;

  // The generation is the asking device's live stream, as the paired endpoints resolve it. A
  // reconnect starts its new session over the old one's, while the old stream's entry stays.
  const std::string device = "reconnecting-tablet-4f1c";
  stream_stats::start_session_timing(device, 392, "first-launch");
  stream_stats::start_session_timing(device, 393, "second-launch");
  const auto asking = nvhttp::requester_session_generation_for_tests(device);
  stream_stats::stop_session_timing(device, 393);
  EXPECT_EQ(asking, 393u) << "the reconnect was resolved to the stream it replaced";
  EXPECT_EQ(nvhttp::requester_session_generation_for_tests(device), 0u) << "a device with no stream has a generation";

  stats.clients.emplace_back();
  stats.clients.back().session_generation = 393;
  const auto reconnected = reason_for(asking);
  EXPECT_EQ(reconnected.rfind("PyroWave has not encoded a captured frame yet", 0), 0u)
    << "the reconnect was answered about the stream it replaced: " << reconnected;
  for (const auto &reason : {watcher, owner, reconnected, reason_for(0)}) {
    EXPECT_EQ(reason.find("pyrowave_route"), std::string::npos) << "Nova never reads that field: " << reason;
  }
}

namespace {
  /// A session registering and starting the way rtsp_stream::start() does: both start writes carry
  /// the negotiated codec and no encoder yet.
  void start_stream(const std::string &ip, const std::string &name, std::uint64_t generation, const std::string &codec) {
    stream_stats::add_client(ip, name, generation);
    stream_stats::update_video_stats(ip, 0.0, 20000, 0.0, codec, 1920, 1080, {}, generation);
    stream_stats::update_video_stats(0.0, 20000, 0.0, codec, 1920, 1080, {}, generation);
  }

  /// One sample from a session's encode loop, which also writes the process-wide codec and encoder.
  void sample_encoder(std::uint64_t generation, const std::string &codec, const std::string &encoder) {
    stream_stats::update_video_stats(60.0, 20000, 4.0, codec, 1920, 1080, encoder, generation);
  }

  /// The encoder block /polaris/v1/session/status serves the client whose stream has this generation.
  nlohmann::json encoder_block_for(const stream_stats::stats_t &stats, std::uint64_t requester) {
    return nvhttp::session_encoder_identity_json_for_tests(stats, "auto", "", false, requester);
  }

  /// The block and the session health served beside it say this stream is PyroWave, with this reason.
  void expect_pyrowave_answer(const stream_stats::stats_t &stats, std::uint64_t requester) {
    SCOPED_TRACE("asked by generation " + std::to_string(requester));
    const auto block = encoder_block_for(stats, requester);
    EXPECT_EQ(block.at("codec"), "pyrowave");
    EXPECT_EQ(block.at("active_backend"), "pyrowave");
    EXPECT_EQ(block.at("effective_backend"), "pyrowave");
    EXPECT_FALSE(block.at("fallback_allowed").get<bool>());
    const auto &selection = block.at("selection");
    EXPECT_EQ(selection.at("policy"), "explicit_codec");
    EXPECT_EQ(selection.at("selected_encoder"), "pyrowave");
    EXPECT_EQ(selection.at("reason"), stream_stats::pyrowave_route_reason(stats, requester));
    const auto health = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Test", requester);
    EXPECT_EQ(health.at("active_encoder"), "pyrowave");
    EXPECT_EQ(health.at("encoder_selection"), selection);
    EXPECT_EQ(health.value("safe_codec", ""), "pyrowave");
  }

  /// The block and the session health say this stream runs a conventional codec, never PyroWave.
  void expect_conventional_answer(const stream_stats::stats_t &stats, std::uint64_t requester,
                                  const std::string &codec, const std::string &encoder) {
    SCOPED_TRACE("asked by generation " + std::to_string(requester));
    const auto block = encoder_block_for(stats, requester);
    EXPECT_EQ(block.at("codec"), codec);
    EXPECT_NE(block.at("active_backend"), "pyrowave");
    EXPECT_NE(block.at("effective_backend"), "pyrowave");
    if (!encoder.empty()) {
      EXPECT_EQ(block.at("active_backend"), encoder);
      EXPECT_EQ(block.at("effective_backend"), encoder);
    }
    const auto &selection = block.at("selection");
    EXPECT_NE(selection.at("policy"), "explicit_codec");
    EXPECT_NE(selection.at("selected_encoder"), "pyrowave");
    EXPECT_EQ(selection.at("reason").get<std::string>().find("PyroWave"), std::string::npos)
      << "a stream that is not PyroWave was given a PyroWave reason: " << selection.at("reason");
    const auto health = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Test", requester);
    EXPECT_NE(health.at("active_encoder"), "pyrowave");
    EXPECT_EQ(health.at("encoder_selection"), selection);
    EXPECT_EQ(health.value("safe_codec", ""), codec);
  }
}  // namespace

/**
 * A PyroWave owner and an HEVC watcher are each answered with their own codec, encoder and selection.
 *
 * Watch Stream runs a session per watcher beside the owner's, each with its own codec and encoder,
 * and the process-wide codec and encoder are whichever encode loop sampled last. The selection took
 * its PyroWave branch from those, so the watcher could be told PyroWave had not encoded a frame yet
 * and the owner could get the conventional Auto reason, depending on which one sampled last.
 */
TEST(NvhttpSessionEncoderTests, APyroWaveOwnerAndAnHevcWatcherEachGetTheirOwnAnswerInEitherStartupOrder) {
  constexpr std::uint64_t owner = 511;
  constexpr std::uint64_t watcher = 512;
  for (const bool owner_first : {true, false}) {
    SCOPED_TRACE(owner_first ? "the owner started first" : "the watcher started first");
    stream_stats::update_stream_active(false);
    auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
    const auto start = [&](std::uint64_t generation) {
      if (generation == owner) {
        start_stream("10.0.0.5", "Deck", owner, "pyrowave");
      } else {
        start_stream("10.0.0.6", "Tablet", watcher, "hevc");
      }
    };
    const auto sample = [&](std::uint64_t generation) {
      if (generation == owner) {
        sample_encoder(owner, "pyrowave", "pyrowave");
        ASSERT_TRUE(stream_stats::record_pyrowave_route(owner, "gpu_upload"));
      } else {
        sample_encoder(watcher, "hevc", "nvenc");
      }
    };
    // The stream that started first samples last, so the process-wide values are its own.
    const auto first = owner_first ? owner : watcher;
    const auto second = owner_first ? watcher : owner;
    start(first);
    start(second);
    sample(second);
    sample(first);

    const auto stats = stream_stats::get_current();
    ASSERT_EQ(stats.clients.size(), 2u);
    EXPECT_EQ(stats.encoder_backend, first == owner ? "pyrowave" : "nvenc")
      << "the process-wide encoder is not the one that sampled last, so this proves nothing";
    expect_pyrowave_answer(stats, owner);
    EXPECT_NE(encoder_block_for(stats, owner).at("selection").at("reason").get<std::string>().find("GPU"),
              std::string::npos) << "the owner was not answered about its own route";
    expect_conventional_answer(stats, watcher, "hevc", "nvenc");
  }
}

/**
 * A reconnect is answered about its new stream before that stream's first frame.
 *
 * The stream it replaced stays listed until its teardown, and the device's session timing already
 * names the new one, which has registered and started with its codec but sampled no encoder and
 * reported no route. Another stream keeps sampling beside it.
 */
TEST(NvhttpSessionEncoderTests, AReconnectIsAnsweredAboutItsNewStreamBeforeItsFirstFrame) {
  stream_stats::update_stream_active(false);
  const std::string deck = "reconnecting-deck-7a21";
  const std::string tablet = "reconnecting-tablet-7a22";
  auto reset = util::fail_guard([&] {
    stream_stats::stop_session_timing(deck, 523);
    stream_stats::stop_session_timing(tablet, 525);
    stream_stats::update_stream_active(false);
  });
  start_stream("10.0.0.5", "Deck", 521, "pyrowave");
  sample_encoder(521, "pyrowave", "pyrowave");
  ASSERT_TRUE(stream_stats::record_pyrowave_route(521, "zero_copy"));
  start_stream("10.0.0.6", "Tablet", 522, "hevc");
  sample_encoder(522, "hevc", "nvenc");

  // The PyroWave Deck reconnects, and the HEVC watcher samples after the new stream starts.
  stream_stats::start_session_timing(deck, 521, "deck-first-launch");
  start_stream("10.0.0.5", "Deck", 523, "pyrowave");
  stream_stats::start_session_timing(deck, 523, "deck-second-launch");
  sample_encoder(522, "hevc", "nvenc");
  auto stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 3u);
  ASSERT_EQ(stats.encoder_backend, "nvenc");
  const auto reconnect = nvhttp::requester_session_generation_for_tests(deck);
  ASSERT_EQ(reconnect, 523u);
  expect_pyrowave_answer(stats, reconnect);
  EXPECT_NE(encoder_block_for(stats, reconnect).at("selection").at("reason"),
            stream_stats::pyrowave_route_reason(stats, 521))
    << "the reconnect was answered with the route of the stream it replaced";
  expect_conventional_answer(stats, 522, "hevc", "nvenc");

  // The HEVC watcher reconnects too, and the PyroWave stream samples after its new stream starts.
  stream_stats::start_session_timing(tablet, 522, "tablet-first-launch");
  start_stream("10.0.0.6", "Tablet", 525, "hevc");
  stream_stats::start_session_timing(tablet, 525, "tablet-second-launch");
  sample_encoder(521, "pyrowave", "pyrowave");
  stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 4u);
  ASSERT_EQ(stats.encoder_backend, "pyrowave");
  ASSERT_EQ(stats.codec, "pyrowave");
  const auto watcher_reconnect = nvhttp::requester_session_generation_for_tests(tablet);
  ASSERT_EQ(watcher_reconnect, 525u);
  expect_conventional_answer(stats, watcher_reconnect, "hevc", "");
  expect_pyrowave_answer(stats, reconnect);
  expect_pyrowave_answer(stats, 521);
}

namespace {
  /// The Doctor the web console reads from the live stream stats, as /api/stats/stream and its SSE
  /// variant serve it. The blocks they add after it read the settings file, and say nothing here.
  nlohmann::json console_doctor(const stream_stats::stats_t &stats) {
    return confighttp::stream_stats_json(stats).at("doctor");
  }

  /// The encoder selection that Doctor's advanced evidence carries, which Troubleshooting shows.
  nlohmann::json console_encoder_selection(const stream_stats::stats_t &stats) {
    return console_doctor(stats).at("advanced_evidence").at("encoder_selection");
  }

  /// The Doctor's encoder selection evidence row.
  nlohmann::json encoder_selection_evidence(const nlohmann::json &doctor) {
    for (const auto &item : doctor.at("evidence")) {
      if (item.value("id", "") == "encoder_selection") {
        return item;
      }
    }
    return {};
  }

  /// The selection the session health gives the client whose stream has this generation.
  nlohmann::json client_selection(const stream_stats::stats_t &stats, std::uint64_t generation) {
    return nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Test", generation)
      .at("encoder_selection");
  }
}  // namespace

/**
 * The web console's Doctor shows the encoder selection of the stream it describes.
 *
 * The live stream stats built their Doctor with no session health, so its encoder selection was an
 * empty object, its evidence row said the selection was unavailable, and Troubleshooting's Selection
 * reason read unknown on every host while Nova was told the reason. The Doctor now carries the sole
 * stream's own selection, never the one the process-wide encoder would give, which is whichever
 * encode loop sampled last.
 */
TEST(ConsoleDoctorEncoderSelectionTests, TheSoleStreamIsShownItsOwnSelectionAndTwoThatDifferNone) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  constexpr std::uint64_t owner = 621;
  constexpr std::uint64_t watcher = 622;

  // A PyroWave stream, and a Browser Stream sample after it, which names no session and leaves
  // the process-wide encoder its own.
  start_stream("10.0.0.5", "Deck", owner, "pyrowave");
  sample_encoder(owner, "pyrowave", "pyrowave");
  ASSERT_TRUE(stream_stats::record_pyrowave_route(owner, "zero_copy"));
  sample_encoder(0, "hevc", "nvenc");
  auto stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 1u);
  ASSERT_EQ(stats.encoder_backend, "nvenc") << "the process-wide encoder is the stream's own, so this proves nothing";
  auto doctor = console_doctor(stats);
  const auto pyrowave = doctor.at("advanced_evidence").at("encoder_selection");
  EXPECT_EQ(pyrowave.value("selected_encoder", ""), "pyrowave") << pyrowave.dump();
  EXPECT_EQ(pyrowave.value("policy", ""), "explicit_codec");
  EXPECT_EQ(pyrowave.value("reason", ""), stream_stats::pyrowave_route_reason(stats, owner));
  EXPECT_EQ(pyrowave, client_selection(stats, owner)) << "the console and the stream's own client were told apart";
  const auto row = encoder_selection_evidence(doctor);
  EXPECT_EQ(row.value("value", ""), "pyrowave") << row.dump();
  EXPECT_EQ(row.value("status", ""), "pass");
  EXPECT_EQ(row.value("detail", ""), pyrowave.value("reason", ""));

  // An HEVC watcher joins, and the owner samples last. Two streams have two selections, and the
  // console is shown neither.
  start_stream("10.0.0.6", "Tablet", watcher, "hevc");
  sample_encoder(watcher, "hevc", "nvenc");
  sample_encoder(owner, "pyrowave", "pyrowave");
  stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 2u);
  doctor = console_doctor(stats);
  EXPECT_TRUE(doctor.at("advanced_evidence").at("encoder_selection").empty())
    << doctor.at("advanced_evidence").at("encoder_selection").dump();
  EXPECT_EQ(encoder_selection_evidence(doctor).value("value", ""), "unknown");

  // The owner ends. The watcher is the sole stream, while the process-wide encoder is still the one
  // the owner sampled last.
  stream_stats::remove_client("10.0.0.5", owner);
  stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 1u);
  ASSERT_EQ(stats.encoder_backend, "pyrowave") << "the process-wide encoder is the watcher's own, so this proves nothing";
  const auto hevc = console_encoder_selection(stats);
  EXPECT_NE(hevc.value("selected_encoder", ""), "pyrowave") << hevc.dump();
  EXPECT_NE(hevc.value("policy", ""), "explicit_codec") << hevc.dump();
  EXPECT_EQ(hevc, client_selection(stats, watcher));
}

/**
 * Both routes the web console reads the live stream stats from serve the Doctor with the selection.
 *
 * The tests above run stream_stats_json(). A route that went back to serializing the stats itself
 * would serve the Doctor without it, and they would stay green.
 */
TEST(ConsoleDoctorEncoderSelectionTests, BothStreamStatsRoutesServeThePayloadWithTheSelection) {
  std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / "src/confighttp.cpp");
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();
  ASSERT_FALSE(source.empty());
  for (const std::string_view route : {"  void getStreamStats(resp_https_t response, req_https_t request) {",
                                       "  void getStreamStatsSSE(resp_https_t response, req_https_t request) {"}) {
    const auto begin = source.find(route);
    ASSERT_NE(begin, std::string::npos) << route;
    const auto end = source.find("\n  }\n", begin);
    ASSERT_NE(end, std::string::npos) << route;
    const auto body = source.substr(begin, end - begin);
    EXPECT_NE(body.find("stream_stats_payload(stats)"), std::string::npos) << route;
    EXPECT_EQ(body.find("to_json("), std::string::npos) << route << " serializes the stats without the selection";
  }
  const auto payload = source.find("  std::string stream_stats_payload(const stream_stats::stats_t &stats) {");
  ASSERT_NE(payload, std::string::npos);
  const auto payload_end = source.find("\n  }\n", payload);
  ASSERT_NE(payload_end, std::string::npos);
  EXPECT_NE(source.substr(payload, payload_end - payload).find("augment_stream_stats_json(stream_stats_json(stats), stats)"),
            std::string::npos) << "the payload is not built from the stats the tests above read";
}

/**
 * Streams that agree are shown their selection, and a host with nothing streaming is shown its own.
 */
TEST(ConsoleDoctorEncoderSelectionTests, StreamsThatAgreeAndAnIdleHostAreShownTheHostsSelection) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });

  // Nothing streams: the selection the host has made, as a client asking with no stream is told it.
  auto stats = stream_stats::get_current();
  ASSERT_TRUE(stats.clients.empty());
  const auto idle = console_encoder_selection(stats);
  EXPECT_TRUE(idle.contains("policy")) << "an idle host showed no selection: " << idle.dump();
  EXPECT_EQ(idle, client_selection(stats, 0));

  // An owner and a Watch Stream watcher on the host's encoder have one selection between them.
  start_stream("10.0.0.5", "Deck", 631, "hevc");
  start_stream("10.0.0.6", "Tablet", 632, "hevc");
  sample_encoder(631, "hevc", "nvenc");
  sample_encoder(632, "hevc", "nvenc");
  stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 2u);
  const auto agreed = console_encoder_selection(stats);
  EXPECT_TRUE(agreed.contains("policy")) << "two streams that agree showed no selection: " << agreed.dump();
  EXPECT_EQ(agreed, client_selection(stats, 631));
  EXPECT_EQ(agreed, client_selection(stats, 632));
}

TEST(NvhttpSessionHealthTests, HealthyShmFallbackRemainsInformational) {
  auto stats = stable_cpu_copy_stats(120.0, 120.0);
  stats.encode_time_ms = 4.0;
  stats.avg_frame_age_ms = 6.0;

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "RetroidPocket6",
    "Control"
  );

  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");
  EXPECT_EQ(health.at("auto_action"), "none");
  EXPECT_TRUE(health.at("capture_cpu_copy").get<bool>());
  EXPECT_FALSE(health.at("capture_pressure").get<bool>());
  EXPECT_FALSE(health.contains("safe_target_fps"));
  EXPECT_FALSE(health.contains("recovery_profile"));
  EXPECT_FALSE(health.at("relaunch_recommended").get<bool>());
  EXPECT_EQ(health.at("doctor").at("status"), "ok");
  EXPECT_EQ(health.at("doctor").at("severity"), "info");
  EXPECT_EQ(health.at("doctor").at("traffic_light"), "green");
  EXPECT_EQ(health.at("doctor").at("primary_issue"), "none");
}

TEST(NvhttpSessionHealthTests, OverBudgetShmEncoderUsesActualNinetySevenFpsBudget) {
  auto stats = stable_cpu_copy_stats(97.0, 97.0);
  stats.encode_time_ms = 10.4;
  stats.avg_frame_age_ms = 8.0;

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Linux notebook",
    "Grim Dawn"
  );

  EXPECT_EQ(health.at("grade"), "watch");
  EXPECT_EQ(health.at("primary_issue"), "encoder_load");
  EXPECT_EQ(health.at("limiting_factor"), "encoder");
  EXPECT_FALSE(health.at("capture_pressure").get<bool>());
  EXPECT_EQ(health.at("doctor").at("status"), "needs_action");
  EXPECT_EQ(health.at("doctor").at("severity"), "critical");
  EXPECT_EQ(health.at("doctor").at("traffic_light"), "red");
  EXPECT_EQ(health.at("doctor").at("primary_issue"), "encoder_load");
}

TEST(NvhttpSessionHealthTests, PyroWaveSafeBitrateIsTheBitrateItRunsAt) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.encode_time_ms = 30.0;
  stats.bitrate_kbps = 180000;
  stats.codec = "pyrowave";
  const auto pyrowave = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Control");
  ASSERT_NE(pyrowave.at("grade"), "good");
  EXPECT_EQ(pyrowave.at("safe_bitrate_kbps"), 180000);

  // The same stream in HEVC gets the usual cut for a stream that is not good.
  stats.codec = "hevc";
  const auto hevc = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Control");
  EXPECT_EQ(hevc.at("safe_bitrate_kbps"), 135000);
}

TEST(NvhttpSessionHealthTests, MeaningfulTargetMissRemainsHostRenderLimited) {
  const auto health = nvhttp::build_session_health_json_for_tests(
    stable_gpu_native_stats(54.0, 60.0),
    false,
    "Nova Client",
    "Mouse"
  );

  EXPECT_EQ(health.at("grade"), "watch");
  EXPECT_EQ(health.at("primary_issue"), "host_render_limited");
  EXPECT_EQ(health.at("limiting_factor"), "host_render");
  EXPECT_TRUE(health.at("host_render_limited").get<bool>());
}

TEST(NvhttpSessionHealthTests, DuplicateOnlyTargetRateDeliveryDoesNotInventPacing) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.duplicate_frame_ratio = 0.10;

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Nova Client",
    "Control"
  );

  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");
  EXPECT_EQ(health.at("auto_action"), "none");
  EXPECT_FALSE(health.at("host_render_limited").get<bool>());
}

TEST(NvhttpSessionHealthTests, DroppedFramesAtTargetRemainHostRenderLimited) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.dropped_frame_ratio = 0.04;

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Nova Client",
    "Control"
  );

  EXPECT_EQ(health.at("grade"), "watch");
  EXPECT_EQ(health.at("primary_issue"), "host_render_limited");
  EXPECT_EQ(health.at("limiting_factor"), "host_render");
  EXPECT_EQ(health.at("auto_action"), "lower_render_profile");
  EXPECT_TRUE(health.at("host_render_limited").get<bool>());
}

TEST(StreamNetworkStatsTests, OneTransientReportDoesNotClassifyCleanStreamAsNetworkLimited) {
  constexpr auto client_ip = "203.0.113.120";
  stream_stats::update_stream_active(false);
  stream_stats::add_client(client_ip, "RetroidPocket6");

  for (int i = 0; i < 6; ++i) {
    stream::record_network_stats(client_ip, 3.0, 0.0, 0);
  }

  // One ENet control-channel estimate above the cut is noise, not sustained
  // pressure. The old handler submitted this same report twice and defeated
  // the tracker's two-report debounce.
  stream::record_network_stats(client_ip, 3.0, 3.0, 0);
  auto stats = stream_stats::get_current();
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.fps = 120.0;
  stats.encode_target_fps = 120.0;
  stats.codec = "hevc";

  EXPECT_FALSE(stats.network_risk);
  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "RetroidPocket6",
    "Synthetic Frame Counter"
  );
  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");

  // More high control-channel estimates still cannot manufacture media loss.
  stream::record_network_stats(client_ip, 3.0, 3.0, 0);
  EXPECT_FALSE(stream_stats::get_current().network_risk);
  EXPECT_FALSE(stream_stats::get_current().packet_loss_available);
  EXPECT_DOUBLE_EQ(stream_stats::get_current().control_channel_packet_loss, 3.0);

  stream_stats::remove_client(client_ip);
  stream_stats::update_stream_active(false);
}

TEST(StreamNetworkStatsTests, SecondaryClientReportDoesNotReplacePrimaryTelemetry) {
  constexpr auto primary_ip = "203.0.113.121";
  constexpr auto secondary_ip = "203.0.113.122";
  stream_stats::update_stream_active(false);
  stream_stats::add_client(primary_ip, "Primary client");
  stream_stats::add_client(secondary_ip, "Secondary client");

  stream::record_network_stats(primary_ip, 3.0, 0.0, 1000);
  for (int i = 0; i < 50; ++i) {
    stream::record_network_stats(secondary_ip, 90.0, 8.0, 2000);
  }

  const auto stats = stream_stats::get_current();
  EXPECT_DOUBLE_EQ(stats.latency_ms, 3.0);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);
  EXPECT_FALSE(stats.network_risk);
  EXPECT_EQ(stats.control_channel_samples, 1u);
  EXPECT_EQ(stats.bytes_sent, 1000u);
  EXPECT_EQ(stats.clients.size(), 2u);
  if (stats.clients.size() == 2u) {
    EXPECT_DOUBLE_EQ(stats.clients[0].latency_ms, 3.0);
    EXPECT_DOUBLE_EQ(stats.clients[0].packet_loss, 0.0);
    EXPECT_DOUBLE_EQ(stats.clients[0].control_channel_packet_loss, 0.0);
    EXPECT_EQ(stats.clients[0].bytes_sent, 1000u);
    EXPECT_DOUBLE_EQ(stats.clients[1].latency_ms, 90.0);
    EXPECT_DOUBLE_EQ(stats.clients[1].packet_loss, 0.0);
    EXPECT_DOUBLE_EQ(stats.clients[1].control_channel_packet_loss, 8.0);
    EXPECT_EQ(stats.clients[1].bytes_sent, 2000u);
  }

  stream_stats::remove_client(primary_ip);
  stream_stats::remove_client(secondary_ip);
  stream_stats::update_stream_active(false);
}

TEST(StreamNetworkStatsTests, SameAddressReconnectKeepsReplacementSessionActive) {
  constexpr auto client_ip = "203.0.113.123";
  constexpr std::uint64_t old_generation = 41;
  constexpr std::uint64_t replacement_generation = 42;

  stream_stats::update_stream_active(false);
  stream_stats::add_client(client_ip, "RetroidPocket6", old_generation);
  stream_stats::add_client(client_ip, "RetroidPocket6", replacement_generation);

  auto overlapping = stream_stats::get_current();
  ASSERT_EQ(overlapping.clients.size(), 2u);
  EXPECT_TRUE(overlapping.streaming);

  stream_stats::remove_client(client_ip, old_generation);

  const auto replacement = stream_stats::get_current();
  ASSERT_EQ(replacement.clients.size(), 1u);
  EXPECT_EQ(replacement.clients.front().session_generation, replacement_generation);
  EXPECT_TRUE(replacement.streaming);
  EXPECT_EQ(replacement.client_ip, client_ip);

  stream_stats::remove_client(client_ip, replacement_generation);
  stream_stats::update_stream_active(false);
}

TEST(ProcHostPauseClassificationTests, HighRefreshNearTargetDeliveryRemainsSteady) {
  const auto classification = proc::classify_host_pause_session_for_tests(
    stable_gpu_native_stats(115.6, 120.0),
    120.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "good");
  EXPECT_EQ(classification.at("primary_issue"), "steady");
  EXPECT_FALSE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcHostPauseClassificationTests, MeaningfulTargetMissRemainsHostRenderLimited) {
  const auto classification = proc::classify_host_pause_session_for_tests(
    stable_gpu_native_stats(54.0, 60.0),
    60.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "watch");
  EXPECT_EQ(classification.at("primary_issue"), "host_render_limited");
  EXPECT_TRUE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcHostPauseClassificationTests, DuplicateOnlyTargetRateDeliveryDoesNotInventPacing) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.duplicate_frame_ratio = 0.10;

  const auto classification = proc::classify_host_pause_session_for_tests(
    stats,
    60.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "good");
  EXPECT_EQ(classification.at("primary_issue"), "steady");
  EXPECT_FALSE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcHostPauseClassificationTests, AnAbruptDisconnectKeepsItsNetworkCause) {
  // The pause runs at teardown, after the control stream has gone and its readings have stopped, so
  // the freshness gates a live reading needs would grade every abrupt disconnect as a clean network.
  // It reads the verdict the window last judged.
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.network_sample_revision = 40;
  stats.network_last_received_age_ms = 9000;
  stats.media_loss_sample_revision = 38;
  stats.media_loss_last_received_age_ms = 9500;
  stats.network_verdict.loss_available = true;
  stats.network_verdict.loss_pct = 3.1;
  stats.network_verdict.loss_elevated = true;
  stats.network_verdict.risk = true;
  ASSERT_FALSE(stream_stats::judged_network(stats).risk);

  const auto classification = proc::classify_host_pause_session_for_tests(stats, 60.0, false);

  EXPECT_EQ(classification.at("primary_issue"), "network_jitter");
  EXPECT_EQ(classification.at("health_grade"), "watch");
}

TEST(ProcHostPauseClassificationTests, DroppedFramesAtTargetRemainHostRenderLimited) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.dropped_frame_ratio = 0.04;

  const auto classification = proc::classify_host_pause_session_for_tests(
    stats,
    60.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "watch");
  EXPECT_EQ(classification.at("primary_issue"), "host_render_limited");
  EXPECT_TRUE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcSessionLifecycleTests, TerminatedPausedAppPublishesStreamEndedWhenNoSessionsRemain) {
  EXPECT_TRUE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "paused"));
}

TEST(ProcSessionLifecycleTests, OwnerCancelPublishesOnlyAfterCleanupAndAllStreamsFinish) {
  EXPECT_TRUE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "tearing_down"));
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 1, "tearing_down"));
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "tearing_down", false));
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "paused", false));
}

TEST(ProcSessionLifecycleTests, RetriedCleanupCanFinishWithoutTheAppRecord) {
  EXPECT_TRUE(proc::should_publish_stream_ended_after_terminate_for_tests(false, 0, "tearing_down"));
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(false, 0, "tearing_down", false));
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(false, 0, "paused"));
}

TEST(ProcSessionLifecycleTests, TerminatedAppDoesNotPublishStreamEndedWhileClientIsConnected) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 1, "streaming"));
}

TEST(ProcSessionLifecycleTests, AlreadyIdleTerminateDoesNotPublishDuplicateStreamEnded) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "idle"));
}

TEST(ProcSessionLifecycleTests, StreamingTerminateWaitsForStreamJoinToPublishStreamEnded) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "streaming"));
}

// Peer-supplied lengths on the control channel. Everything below arrives off
// the wire from a client that has established a session, so each of these
// bounds is the only thing standing between a declared size and a read.

TEST(ControlPacketBounds, APacketShorterThanItsTypeFieldIsRejected) {
  // dataLength - sizeof(type) wraps below 2, so the payload view would claim
  // the entire address space.
  EXPECT_FALSE(stream::control_packet_carries_type(0));
  EXPECT_FALSE(stream::control_packet_carries_type(1));
  EXPECT_TRUE(stream::control_packet_carries_type(2));
  EXPECT_TRUE(stream::control_packet_carries_type(1024));
}

TEST(ControlPacketBounds, EncryptedHeaderMustHaveArrived) {
  // The dispatcher consumed the 2-byte type, so 6 payload bytes complete the
  // 8-byte header the handler reads length and seq from.
  EXPECT_FALSE(stream::encrypted_control_header_present(0));
  EXPECT_FALSE(stream::encrypted_control_header_present(5));
  EXPECT_TRUE(stream::encrypted_control_header_present(6));
}

TEST(ControlPacketBounds, EncryptedCipherLengthIsBoundedByWhatArrived) {
  // 24 is the runt floor; the cipher is length - 4 bytes past the header.
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(6, 23));  // below the floor
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(6, 24));  // declares 20, none arrived
  EXPECT_TRUE(stream::encrypted_control_cipher_fits(26, 24));  // declares 20, 20 arrived
  EXPECT_TRUE(stream::encrypted_control_cipher_fits(1024, 24));

  // A uint16 maxes out at 65535, so the over-read was bounded but real.
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(64, 65535));
}

TEST(ControlPacketBounds, InputCipherLengthRejectsNegativeAndOverlongClaims) {
  EXPECT_FALSE(stream::input_control_cipher_fits(3, 0));   // no room for the length itself
  EXPECT_TRUE(stream::input_control_cipher_fits(4, 0));
  EXPECT_TRUE(stream::input_control_cipher_fits(20, 16));
  EXPECT_FALSE(stream::input_control_cipher_fits(20, 17));  // one byte past the buffer

  // Signed off the wire: -1 would widen to SIZE_MAX as a view length.
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, -1));
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, std::numeric_limits<std::int32_t>::min()));
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, std::numeric_limits<std::int32_t>::max()));
}


TEST(NvhttpSessionHealthTests, PyrowaveReportsItsSessionEncoderWithoutNvencWarnings) {
  auto stats = stable_cpu_copy_stats(90.0, 90.0);
  stats.codec = "pyrowave";
  stats.encoder_backend = "pyrowave";
  stats.encode_time_ms = 3.0;
  stats.avg_frame_age_ms = 6.0;
  const auto health = nvhttp::build_session_health_json_for_tests(stats, false, "Nova Client", "Control");
  EXPECT_EQ(health.at("active_encoder"), "pyrowave");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("encoder_selection").at("selected_encoder"), "pyrowave");
  EXPECT_EQ(health.at("encoder_selection").at("preferred_encoder"), "pyrowave");
  EXPECT_FALSE(health.at("encoder_selection").at("fallback_used").get<bool>());
}

TEST(StreamStartOutcomeTests, AClientThatConnectedAndLeftBeforeAnyPingStoppedDuringSetup) {
  // An Android TV client negotiated PyroWave, could not build the decoder, and left 113 ms after its
  // control stream connected. The host logged two ping timeouts ten seconds later, which read as a
  // network fault.
  stream_start::control_timeline_t timeline;
  timeline.connected = true;
  timeline.disconnected = true;
  timeline.connected_for = std::chrono::milliseconds {113};
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_client_left_during_setup);

  const auto line = stream_start::client_left_during_setup_message("Living Room TV", "pyrowave", timeline.connected_for);
  EXPECT_EQ(line,
            "Stream failed to start for [Living Room TV]: the client left during video setup 113 ms after "
            "connecting, before any video or audio arrived (codec PyroWave)");
  EXPECT_EQ(line.find("Initial Ping Timeout"), std::string::npos);

  // A ping before the disconnect means the client got past its own setup.
  timeline.any_ping = true;
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_no_ping);
  const auto after_ping = stream_start::ping_timeout_message("video", 47998, std::chrono::milliseconds {10000}, timeline);
  EXPECT_NE(after_ping.find("disconnected 113 ms after connecting"), std::string::npos) << after_ping;
  EXPECT_EQ(after_ping.find("firewall"), std::string::npos) << "the client left; the path did not fail: " << after_ping;
}

TEST(StreamStartOutcomeTests, AClientThatStayedConnectedAndNeverPingedIsARealPingTimeout) {
  stream_start::control_timeline_t timeline;
  timeline.connected = true;
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_no_ping);
  const auto stayed = stream_start::ping_timeout_message("video", 47998, std::chrono::milliseconds {10000}, timeline);
  EXPECT_EQ(stayed,
            "Initial Ping Timeout: no video ping arrived on UDP 47998 within 10000 ms while the client's control "
            "stream stayed connected. A firewall or a UDP path problem between the client and this host usually does that");

  // A client whose control stream never arrived either is the same kind of path problem.
  timeline.connected = false;
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_no_ping);
  const auto never = stream_start::ping_timeout_message("audio", 48000, std::chrono::milliseconds {10000}, timeline);
  EXPECT_NE(never.find("no audio ping arrived on UDP 48000"), std::string::npos) << never;
  EXPECT_NE(never.find("never connected its control stream"), std::string::npos) << never;
  EXPECT_NE(never.find("firewall or a UDP path problem"), std::string::npos) << never;

  // A disconnect without a connect is not a client that left during setup.
  timeline.disconnected = true;
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_no_ping);
}

TEST(StreamStartOutcomeTests, AClientThatLeftAfterWaitingForVideoIsARealPingTimeout) {
  // A client whose pings never reach the host waits for video and then leaves; moonlight-common-c
  // gives up after ten seconds without any. With ping_timeout raised past that, the host sees the
  // client leave before its own wait ends. That is the UDP path, and a PyroWave stream behind a
  // blocked port must not be told its decoder failed.
  stream_start::control_timeline_t timeline;
  timeline.connected = true;
  timeline.disconnected = true;
  timeline.connected_for = std::chrono::milliseconds {10050};
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_no_ping);
  EXPECT_EQ(stream_start::ping_timeout_message("video", 47998, std::chrono::milliseconds {30000}, timeline),
            "Initial Ping Timeout: no video ping arrived on UDP 47998 within 30000 ms, and the client disconnected "
            "10050 ms after connecting. A firewall or a UDP path problem between the client and this host usually does that");

  // The limit splits the two: a quick leave is the client's own setup, a late one is the path.
  timeline.connected_for = stream_start::k_setup_leave_limit - std::chrono::milliseconds {1};
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_client_left_during_setup);
  timeline.connected_for = stream_start::k_setup_leave_limit;
  EXPECT_EQ(stream_start::classify_ping_timeout(timeline), stream_start::k_no_ping);
}

TEST(StreamStartOutcomeTests, TheSessionStartLineNamesWhatTheClientNegotiated) {
  EXPECT_EQ(stream_start::describe_negotiated_stream({
              .client = "Living Room TV",
              .codec = "pyrowave",
              .dynamic_range = 0,
              .chroma_sampling = 0,
              .width = 1920,
              .height = 1080,
              .fps = 60.0,
            }),
            "Stream negotiated for [Living Room TV]: PyroWave, 8-bit, 4:2:0, 1920x1080 at 60 fps");
  EXPECT_EQ(stream_start::describe_negotiated_stream({
              .client = "Deck",
              .codec = "hevc",
              .dynamic_range = 1,
              .chroma_sampling = 1,
              .width = 2560,
              .height = 1440,
              .fps = 60000.0 / 1001.0,
            }),
            "Stream negotiated for [Deck]: HEVC, 10-bit, 4:4:4, 2560x1440 at 59.94 fps");
  EXPECT_EQ(stream_start::codec_label("h264"), "H.264");
  EXPECT_EQ(stream_start::codec_label("av1"), "AV1");
  EXPECT_EQ(stream_start::codec_label("vp9"), "");
}

TEST(StreamStartOutcomeTests, TheNextStepNamesPyroWaveOnlyForPyroWave) {
  EXPECT_EQ(stream_start::failed_start_next_step("pyrowave"),
            "The client could not start its PyroWave decoder. Choose HEVC or H.264 for that device, or update the client.");
  for (const auto *codec : {"h264", "hevc", "av1", ""}) {
    EXPECT_EQ(stream_start::failed_start_next_step(codec),
              "The client stopped during video setup; its own error message names the cause.")
      << codec;
  }
}

TEST(NvhttpSessionHealthTests, EffectiveEncoderUsesNegotiatedPyrowaveBeforeFirstSample) {
  auto stats = stable_cpu_copy_stats(90.0, 90.0);
  stats.codec = "pyrowave";
  stats.encoder_backend.clear();
  EXPECT_EQ(nvhttp::effective_session_encoder_name_for_tests(stats, "nvenc"), "pyrowave");
  stats.encoder_backend = "pyrowave";
  EXPECT_EQ(nvhttp::effective_session_encoder_name_for_tests(stats, "nvenc"), "pyrowave");
  stats.codec = "hevc";
  stats.encoder_backend = "vaapi";
  EXPECT_EQ(nvhttp::effective_session_encoder_name_for_tests(stats, "nvenc"), "vaapi");
  stats.encoder_backend.clear();
  EXPECT_EQ(nvhttp::effective_session_encoder_name_for_tests(stats, "nvenc"), "nvenc");
  stats.codec = "pyrowave";
  stats.streaming = false;
  EXPECT_EQ(nvhttp::effective_session_encoder_name_for_tests(stats, "nvenc"), "nvenc");
}
