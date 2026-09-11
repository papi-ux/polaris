/**
 * @file tests/unit/platform/test_multiseat_worker_media_pump.cpp
 * @brief Contract tests for the worker media pump.
 */
#include "multiseat_fake_worker.h"

#include "src/platform/linux/multiseat_worker_media_pump.h"

#include <gtest/gtest.h>

#ifdef __linux__

  #include <algorithm>
  #include <atomic>
  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <chrono>
  #include <thread>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <utility>
  #include <vector>

namespace {
  using namespace multiseat::worker_ipc;
  using namespace multiseat::media;
  using namespace multiseat_test;
  using namespace std::chrono_literals;

  /** What the fake worker announces, and what a client asking for it expects. */
  expected_media_t matching_expectation() {
    return {
      .width = 1280,
      .height = 720,
      .fps = 60,
      .video_format = 0,
      .audio_channels = 2,
    };
  }

  /** Everything the sinks received, in arrival order. */
  struct delivered_t {
    struct video_t {
      std::vector<std::uint8_t> bytes;
      std::int64_t index = 0;
      bool idr = false;
    };

    mutable std::mutex mutex;
    std::vector<video_t> video;
    std::vector<std::vector<std::uint8_t>> audio;

    delivery_sinks_t sinks() {
      return {
        .video = [this](std::vector<std::uint8_t> &&bytes, std::int64_t index, bool idr) {
          std::scoped_lock lock {mutex};
          video.push_back({.bytes = std::move(bytes), .index = index, .idr = idr});
        },
        .audio = [this](std::vector<std::uint8_t> &&bytes) {
          std::scoped_lock lock {mutex};
          audio.push_back(std::move(bytes));
        },
      };
    }
  };

  /** A host that asks for nothing and never stops the stream. */
  host_requests_t quiet_host() {
    return {
      .stop_requested = [] { return false; },
      .take_idr_request = [] { return false; },
      .take_invalidation = [] { return std::optional<std::pair<std::int64_t, std::int64_t>> {}; },
    };
  }

  std::string read_source(const std::string &relative) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  /** The body of one function, from its signature to the next one named. */
  std::string body_between(const std::string &source, const std::string &from, const std::string &to) {
    const auto start = source.find(from);
    const auto end = source.find(to, start == std::string::npos ? 0 : start);
    if (start == std::string::npos || end == std::string::npos) {
      return {};
    }
    return source.substr(start, end - start);
  }
}  // namespace

/**
 * The pump is only half of the rule. The other half is that a session which
 * has a worker never reaches host capture at all, which lives in the session's
 * own threads and is checked here at the source, because starting a real
 * session needs a client on the other end.
 */
TEST(MultiseatWorkerMediaPump, AWorkerSessionCannotReachHostCapture) {
  const auto source = read_source("src/stream.cpp");
  ASSERT_FALSE(source.empty());

  const auto video_thread = body_between(source, "void videoThread(session_t *session) {", "void audioThread(session_t *session) {");
  ASSERT_FALSE(video_thread.empty());
  const auto worker_branch = video_thread.find("worker_media_thread(session);");
  const auto host_capture = video_thread.find("video::capture(");
  ASSERT_NE(worker_branch, std::string::npos) << "the video thread must hand a worker session to the pump";
  ASSERT_NE(host_capture, std::string::npos);
  EXPECT_LT(worker_branch, host_capture) << "a worker session must leave before it can reach host capture";
  EXPECT_NE(video_thread.find("if (session->worker_connection) {"), std::string::npos);

  const auto audio_thread = body_between(source, "void audioThread(session_t *session) {", "namespace session {");
  ASSERT_FALSE(audio_thread.empty());
  const auto audio_branch = audio_thread.find("if (session->worker_connection) {");
  const auto audio_capture = audio_thread.find("audio::capture(");
  ASSERT_NE(audio_branch, std::string::npos);
  ASSERT_NE(audio_capture, std::string::npos);
  EXPECT_LT(audio_branch, audio_capture);

  const auto pump_thread = body_between(source, "void worker_media_thread(session_t *session) {", "void videoThread(session_t *session) {");
  ASSERT_FALSE(pump_thread.empty());
  EXPECT_EQ(pump_thread.find("video::capture("), std::string::npos)
    << "the worker path must never start host video capture";
  EXPECT_EQ(pump_thread.find("audio::capture("), std::string::npos)
    << "the worker path must never start host audio capture";
}

TEST(MultiseatWorkerMediaPump, RefusesWithoutAConnectionOrSinks) {
  const controller_connection_t empty;
  delivered_t delivered;
  EXPECT_EQ(run(empty, matching_expectation(), delivered.sinks(), quiet_host()).status,
            pump_status_e::no_connection);
  EXPECT_FALSE(ended_cleanly(pump_status_e::no_connection));
  EXPECT_TRUE(ended_cleanly(pump_status_e::ended_on_shutdown));
  EXPECT_TRUE(ended_cleanly(pump_status_e::ended_on_end_of_stream));
  EXPECT_FALSE(describe(pump_status_e::transport_lost).empty());
}

TEST(MultiseatWorkerMediaPump, DeliversAnAcknowledgedContractsFramesToTheStream) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x41)};
  auto authority = create_authority(store, identity_for(), "generation-pump-deliver");
  fake_worker_t worker {authority, fake_behavior_e::media_contract};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);

  delivered_t delivered;
  const auto report = run(
    client.lease_connection(), matching_expectation(), delivered.sinks(), quiet_host()
  );

  // The fake worker announces its contract, sends one video and one audio
  // frame, then stops answering, which ends the stream as a transport loss.
  EXPECT_EQ(report.contract.width, 1280);
  EXPECT_EQ(report.contract.height, 720);
  EXPECT_EQ(report.video_frames, 1U);
  EXPECT_EQ(report.audio_frames, 1U);
  EXPECT_EQ(
    worker.contract_messages(),
    (std::vector<message_e> {message_e::media_config_ack})
  ) << "the pump must acknowledge the contract before taking any frame";

  std::scoped_lock lock {delivered.mutex};
  ASSERT_EQ(delivered.video.size(), 1U);
  EXPECT_EQ(std::string(delivered.video[0].bytes.begin(), delivered.video[0].bytes.end()), "video");
  ASSERT_EQ(delivered.audio.size(), 1U);
  EXPECT_EQ(std::string(delivered.audio[0].begin(), delivered.audio[0].end()), "audio");

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerMediaPump, RefusesAContractTheClientDidNotNegotiate) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x42)};
  auto authority = create_authority(store, identity_for(), "generation-pump-mismatch");
  fake_worker_t worker {authority, fake_behavior_e::media_contract};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);

  auto expectation = matching_expectation();
  expectation.height = 1080;  // the worker announced 720
  delivered_t delivered;
  const auto report = run(
    client.lease_connection(), expectation, delivered.sinks(), quiet_host()
  );

  EXPECT_EQ(report.status, pump_status_e::mismatched_contract);
  EXPECT_EQ(report.video_frames, 0U);
  EXPECT_EQ(report.audio_frames, 0U);
  EXPECT_FALSE(report.detail.empty());
  EXPECT_TRUE(worker.contract_messages().empty())
    << "a contract the client did not negotiate must never be acknowledged";

  std::scoped_lock lock {delivered.mutex};
  EXPECT_TRUE(delivered.video.empty());
  EXPECT_TRUE(delivered.audio.empty());

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerMediaPump, RefusesAFormatNoWorkerCanServe) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x43)};
  auto authority = create_authority(store, identity_for(), "generation-pump-format");
  fake_worker_t worker {authority, fake_behavior_e::media_contract};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);

  auto expectation = matching_expectation();
  expectation.video_format = 1;  // HEVC, which the contract cannot describe
  delivered_t delivered;
  EXPECT_EQ(
    run(client.lease_connection(), expectation, delivered.sinks(), quiet_host()).status,
    pump_status_e::mismatched_contract
  );

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerMediaPump, AFrameBeforeTheContractEndsTheStream) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x44)};
  auto authority = create_authority(store, identity_for(), "generation-pump-early-frame");
  // The plain healthy worker sends video first, with no contract at all.
  fake_worker_t worker {authority};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);

  delivered_t delivered;
  const auto report = run(
    client.lease_connection(), matching_expectation(), delivered.sinks(), quiet_host()
  );

  EXPECT_EQ(report.status, pump_status_e::frame_before_contract);
  EXPECT_EQ(report.video_frames, 0U);
  std::scoped_lock lock {delivered.mutex};
  EXPECT_TRUE(delivered.video.empty());

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerMediaPump, AStoppingSessionEndsTheStreamAndClosesItsTransport) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x45)};
  auto authority = create_authority(store, identity_for(), "generation-pump-stop");
  fake_worker_t worker {authority, fake_behavior_e::media_contract};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);

  std::atomic<bool> stopping {false};
  auto host = quiet_host();
  host.stop_requested = [&] { return stopping.load(); };
  delivered_t delivered;
  auto lease = client.lease_connection();

  std::thread stopper {[&] {
    // Let the contract and its two frames land, then end the session.
    std::this_thread::sleep_for(150ms);
    stopping.store(true);
  }};
  const auto report = run(lease, matching_expectation(), delivered.sinks(), host);
  stopper.join();

  EXPECT_EQ(report.status, pump_status_e::ended_on_shutdown);
  EXPECT_TRUE(ended_cleanly(report.status));
  EXPECT_FALSE(lease.connected()) << "a stopping session must not leave its transport open";

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerMediaPump, CarriesTheHostsKeyframeAndInvalidationAsks) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x46)};
  auto authority = create_authority(store, identity_for(), "generation-pump-asks");
  fake_worker_t worker {authority, fake_behavior_e::media_contract};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);

  std::atomic<bool> stopping {false};
  std::atomic<int> idr_asks {1};
  std::atomic<int> invalidations {1};
  host_requests_t host {
    .stop_requested = [&] { return stopping.load(); },
    .take_idr_request = [&] { return idr_asks.fetch_sub(1) > 0; },
    .take_invalidation = [&]() -> std::optional<std::pair<std::int64_t, std::int64_t>> {
      if (invalidations.fetch_sub(1) > 0) {
        return std::pair<std::int64_t, std::int64_t> {7, 9};
      }
      return {};
    },
  };
  delivered_t delivered;
  std::thread stopper {[&] {
    std::this_thread::sleep_for(200ms);
    stopping.store(true);
  }};
  const auto report = run(
    client.lease_connection(), matching_expectation(), delivered.sinks(), host
  );
  stopper.join();

  EXPECT_EQ(report.status, pump_status_e::ended_on_shutdown);
  EXPECT_EQ(report.idr_requests, 1U);
  EXPECT_EQ(report.invalidations, 1U);
  const auto seen = worker.contract_messages();
  EXPECT_NE(std::find(seen.begin(), seen.end(), message_e::request_idr), seen.end());
  EXPECT_NE(std::find(seen.begin(), seen.end(), message_e::invalidate_ref_frames), seen.end());
  const auto invalidated = worker.invalidated();
  ASSERT_TRUE(invalidated);
  EXPECT_EQ(invalidated->first, 7U);
  EXPECT_EQ(invalidated->last, 9U);

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

#endif
