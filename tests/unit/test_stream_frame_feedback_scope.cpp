/** @file tests/unit/test_stream_frame_feedback_scope.cpp
 * @brief Exercise frame feedback through the production session scope and legacy ingest.
 */
#include <src/stream_stats.h>
#include <src/stream_frame_feedback_json.h>
#include <src/video_bitrate_provenance.h>
#include <src/adaptive_bitrate.h>
#include <src/vrr_trace.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {
  // One authenticated stream scope with the timing lifecycle the production route uses.
  struct media_receipt_scope_t {
    std::string owner;
    std::uint64_t generation;
    std::string token;

    media_receipt_scope_t(std::string owner_uuid, std::uint64_t session_generation, std::string session_token):
        owner(std::move(owner_uuid)), generation(session_generation), token(std::move(session_token)) {
      adaptive_bitrate::set_enabled(false);
      stream_stats::update_stream_active(false);
      stream_stats::start_session_timing(owner, generation, token);
      stream_stats::update_stream_active(true);
    }

    ~media_receipt_scope_t() {
      stream_stats::stop_session_timing(owner, generation);
      stream_stats::update_stream_active(false);
    }

    stream_stats::client_media_counters_t sample(std::uint64_t client_ms, std::uint64_t expected, std::uint64_t received) const {
      return {
        .owner_uuid = owner, .app_session_id = token, .session_generation = generation,
        .client_monotonic_ms = client_ms, .frames_expected = expected, .frames_received = received,
        .frames_lost = expected - received
      };
    }
  };
}  // namespace

namespace {
  // #611: the production stream_stats seam for the optional frame-submission section. Records come
  // from the real provenance ledger, sends through the real sender record, sections through the real
  // wire parser.
  namespace bp = video::bitrate_provenance;
  const std::string k_frame_launch = "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a";
  const std::string k_frame_other_launch = "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5";

  nlohmann::json frame_submission_sample(std::uint64_t sequence, std::uint64_t cursor, std::uint64_t units,
                                         std::uint64_t at_ms, const std::string &launch = k_frame_launch) {
    return {
      {"monotonic_timestamp_ms", at_ms}, {"frames_expected", 100}, {"frames_received", 100}, {"frames_lost", 0},
      {"frame_submission", {
        {"version", 1}, {"launch_nonce", launch}, {"connection_nonce", "0123456789abcdef0123456789abcdef"},
        {"native_epoch", 1}, {"snapshot_sequence", sequence}, {"last_frame_index", cursor},
        {"decode_units_submitted_total", units}, {"payload_bytes_total", units * 100},
        {"monotonic_timestamp_ms", at_ms}, {"last_submission_timestamp_ms", at_ms},
      }},
    };
  }

  void send_encoded_frames(stream_frame_feedback::sender_ledger_t &ledger, std::uint64_t generation,
                           std::uint64_t lifetime, int kbps, int first, int last) {
    const bp::scope_t scope {lifetime, generation};
    bp::ledger_t encoder {scope};
    ASSERT_TRUE(encoder.observe_receipt({scope, 1, 1, kbps, bp::receipt_origin_e::successful_open}));
    for (int frame = first; frame <= last; ++frame) {
      auto submission = encoder.capture(frame);
      ASSERT_TRUE(submission);
      ASSERT_TRUE(encoder.complete(*submission, bp::submission_result_e::submitted_under_captured_setting));
      const auto record = encoder.take(scope, frame);
      ASSERT_TRUE(record);
      stream_frame_feedback::frame_send_record_t sent {&ledger, frame, std::optional<const bp::record_t> {*record}};
      sent.send_returned(true, true);
      sent.path_finished(false);
    }
  }

  stream_frame_feedback::receipt_t ingest_frame(const std::string &owner, const std::string &token,
                                                std::uint64_t generation, const nlohmann::json &sample) {
    return stream_stats::ingest_frame_submission(owner, token, generation, stream_frame_feedback::parse_section(sample));
  }
}  // namespace

TEST(StreamStatsFrameSubmission, JoinsOnlyInTheExactScopeWithTraceOffAndNeverMovesLegacyOrPolicy) {
  ASSERT_FALSE(vrr_trace::enabled()) << "the sender ledger must work with tracing off";
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  constexpr std::uint64_t generation = 6111;
  auto ledger = std::make_shared<stream_frame_feedback::sender_ledger_t>(generation);
  stream_stats::start_session_timing("frame-owner", generation, "frame-app", ledger, k_frame_launch);
  stream_stats::update_stream_active(true);
  send_encoded_frames(*ledger, generation, 71, 18'000, 1, 8);

  const auto policy_before = adaptive_bitrate::get_state_with_history();
  const auto stats_before = stream_stats::get_current();
  EXPECT_EQ(std::string {ingest_frame("frame-owner", "frame-app", generation, frame_submission_sample(1, 2, 2, 1'000)).reason},
            "baseline");
  const auto joined = ingest_frame("frame-owner", "frame-app", generation, frame_submission_sample(2, 8, 7, 1'100));
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.frames_emitted, 6u);
  EXPECT_EQ(joined.decode_units_submitted, 5u);
  EXPECT_EQ(joined.frames_absent_at_submission, 1u);
  EXPECT_EQ(joined.encoder_setting.configured_encoder_kbps, 18'000);
  // Any other owner, app session or generation is refused before the section is read.
  EXPECT_EQ(std::string {ingest_frame("other-owner", "frame-app", generation, frame_submission_sample(3, 8, 7, 1'200)).reason}, "scope_mismatch");
  EXPECT_EQ(std::string {ingest_frame("frame-owner", "other-app", generation, frame_submission_sample(3, 8, 7, 1'200)).reason}, "scope_mismatch");
  EXPECT_EQ(std::string {ingest_frame("frame-owner", "frame-app", generation + 1, frame_submission_sample(3, 8, 7, 1'200)).reason}, "scope_mismatch");

  // The readback is the same scope's newest receipt; legacy evidence and Live Tuning did not move.
  ASSERT_EQ(stream_stats::get_single_active_session_identity().value_or(stream_stats::active_session_identity_t {}).session_generation,
            generation);
  const auto stats = stream_stats::get_current();
  ASSERT_TRUE(stats.frame_submission);
  EXPECT_EQ(stats.frame_submission->session_generation, generation);
  const auto interval = stream_stats::client_media_interval_json(stats);
  EXPECT_FALSE(interval.at("available").get<bool>()) << "no legacy receipt was made by the new section";
  EXPECT_EQ(interval.at("media_rate_attribution").at("state"), "unknown");
  const auto &receipt = interval.at("frame_submission_receipt");
  EXPECT_EQ(receipt.at("reason"), "joined");
  EXPECT_EQ(receipt.at("frames_emitted"), 6);
  EXPECT_EQ(receipt.at("encoder_setting").at("configured_encoder_kbps"), 18'000);
  EXPECT_EQ(receipt.dump().find("frame-app"), std::string::npos);
  EXPECT_EQ(receipt.dump().find(k_frame_launch), std::string::npos);
  EXPECT_EQ(stats.packet_loss_available, stats_before.packet_loss_available);
  EXPECT_DOUBLE_EQ(stats.packet_loss, stats_before.packet_loss);
  const auto policy_after = adaptive_bitrate::get_state_with_history();
  EXPECT_EQ(policy_after.history.entries.size(), policy_before.history.entries.size());
  EXPECT_EQ(policy_after.target_bitrate_kbps, policy_before.target_bitrate_kbps);

  // The owner status reads its nonce from this same timing scope.
  const auto timing = stream_stats::get_session_timing("frame-owner");
  EXPECT_EQ(timing.session_generation, generation);
  EXPECT_EQ(timing.session_token, "frame-app");
  EXPECT_EQ(timing.frame_submission_launch_nonce, k_frame_launch);

  stream_stats::stop_session_timing("frame-owner", generation);
  EXPECT_EQ(std::string {ingest_frame("frame-owner", "frame-app", generation, frame_submission_sample(3, 8, 7, 1'200)).reason}, "scope_mismatch");
  EXPECT_EQ(std::string {ledger->join(2, 8, std::chrono::steady_clock::now(), std::chrono::seconds(5)).reason}, "scope_retired");
  EXPECT_FALSE(stream_stats::get_current().frame_submission);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsFrameSubmission, AResumeUnderTheSameAppTokenBindsOnlyItsOwnNewLaunchNonce) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  auto first = std::make_shared<stream_frame_feedback::sender_ledger_t>(6121);
  stream_stats::start_session_timing("resume-owner", 6121, "resume-app", first, k_frame_launch);
  stream_stats::update_stream_active(true);
  send_encoded_frames(*first, 6121, 81, 15'000, 1, 4);
  EXPECT_EQ(std::string {ingest_frame("resume-owner", "resume-app", 6121, frame_submission_sample(1, 2, 2, 1'000)).reason}, "baseline");

  // Resume: same owner and app token, a new generation admitted with a fresh nonce.
  auto second = std::make_shared<stream_frame_feedback::sender_ledger_t>(6122);
  stream_stats::start_session_timing("resume-owner", 6122, "resume-app", second, k_frame_other_launch);
  send_encoded_frames(*second, 6122, 82, 15'000, 1, 4);
  EXPECT_EQ(std::string {first->join(0, 4, std::chrono::steady_clock::now(), std::chrono::seconds(5)).reason}, "scope_retired");
  EXPECT_EQ(std::string {ingest_frame("resume-owner", "resume-app", 6121, frame_submission_sample(2, 4, 4, 1'100)).reason}, "scope_mismatch");
  // A late report carrying the replaced launch's nonce never binds the resumed scope.
  EXPECT_EQ(std::string {ingest_frame("resume-owner", "resume-app", 6122, frame_submission_sample(1, 2, 2, 1'000, k_frame_launch)).reason},
            "launch_nonce_mismatch");
  EXPECT_EQ(std::string {ingest_frame("resume-owner", "resume-app", 6122, frame_submission_sample(2, 2, 2, 1'050, k_frame_other_launch)).reason},
            "baseline");
  EXPECT_EQ(stream_stats::get_session_timing("resume-owner").frame_submission_launch_nonce, k_frame_other_launch);

  // A scope admitted with an invalid or no nonce keeps none and binds nothing.
  stream_stats::start_session_timing("resume-owner", 6123, "resume-app",
    std::make_shared<stream_frame_feedback::sender_ledger_t>(6123), "5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A");
  EXPECT_TRUE(stream_stats::get_session_timing("resume-owner").frame_submission_launch_nonce.empty());
  EXPECT_EQ(std::string {ingest_frame("resume-owner", "resume-app", 6123, frame_submission_sample(1, 2, 2, 1'000)).reason},
            "launch_nonce_unavailable");
  // A ledger of another generation is never bound to this scope.
  stream_stats::start_session_timing("resume-owner", 6124, "resume-app",
    std::make_shared<stream_frame_feedback::sender_ledger_t>(9999), k_frame_launch);
  EXPECT_EQ(std::string {ingest_frame("resume-owner", "resume-app", 6124, frame_submission_sample(1, 2, 2, 1'000)).reason},
            "sender_ledger_unavailable");
  stream_stats::stop_session_timing("resume-owner", 6124);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsFrameSubmission, ReconnectRacingIngestNeverCrossesGenerations) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  constexpr std::uint64_t old_generation = 6131, new_generation = 6132;
  auto old_ledger = std::make_shared<stream_frame_feedback::sender_ledger_t>(old_generation);
  stream_stats::start_session_timing("race-owner", old_generation, "race-app", old_ledger, k_frame_launch);
  stream_stats::update_stream_active(true);
  send_encoded_frames(*old_ledger, old_generation, 91, 20'000, 1, 400);
  ASSERT_EQ(std::string {ingest_frame("race-owner", "race-app", old_generation, frame_submission_sample(1, 1, 1, 1'000)).reason}, "baseline");

  std::atomic<bool> go {false};
  auto new_ledger = std::make_shared<stream_frame_feedback::sender_ledger_t>(new_generation);
  std::thread reconnect([&] {
    while (!go.load()) std::this_thread::yield();
    stream_stats::start_session_timing("race-owner", new_generation, "race-app", new_ledger, k_frame_launch);
  });
  go.store(true);
  bool replaced = false;
  for (std::uint64_t sequence = 2; sequence <= 400; ++sequence) {
    const auto receipt = ingest_frame("race-owner", "race-app", old_generation,
                                      frame_submission_sample(sequence, sequence, sequence, 1'000 + sequence));
    const std::string why = receipt.reason;
    if (why == "scope_mismatch") {
      replaced = true;
      continue;
    }
    // Once the reconnect won, no later report for the old scope can be folded.
    EXPECT_FALSE(replaced) << why;
    EXPECT_TRUE(receipt.available) << why;
  }
  reconnect.join();
  EXPECT_EQ(std::string {ingest_frame("race-owner", "race-app", old_generation, frame_submission_sample(401, 400, 400, 1'401)).reason},
            "scope_mismatch");
  // The new scope starts clean: nothing from the old one is its baseline or its readback.
  const auto stats = stream_stats::get_current();
  EXPECT_FALSE(stats.frame_submission);
  EXPECT_EQ(std::string {ingest_frame("race-owner", "race-app", new_generation, frame_submission_sample(1, 1, 1, 5'000)).reason},
            "cursor_beyond_sender_high_water");
  stream_stats::stop_session_timing("race-owner", new_generation);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsFrameSubmission, AMalformedSectionLeavesTheLegacyIngestExactlyAsItWas) {
  media_receipt_scope_t scope("frame-legacy-owner", 6141, "frame-legacy-app");
  const auto baseline = stream_stats::ingest_client_media_counters(scope.sample(1'000, 100, 100));
  ASSERT_EQ(baseline.state, stream_stats::client_media_ingest_state_e::baseline);
  auto malformed = frame_submission_sample(1, 2, 2, 1'000);
  malformed["frame_submission"]["native_epoch"] = 1.5;
  const auto refused = ingest_frame(scope.owner, scope.token, scope.generation, malformed);
  EXPECT_FALSE(refused.available);
  EXPECT_EQ(std::string {refused.reason}, "malformed_native_epoch");

  const auto observed = stream_stats::ingest_client_media_counters(scope.sample(2'250, 200, 196));
  EXPECT_EQ(observed.state, stream_stats::client_media_ingest_state_e::observed);
  EXPECT_TRUE(observed.accepted);
  EXPECT_TRUE(observed.observation_published);
  EXPECT_DOUBLE_EQ(observed.media_loss_pct, 4.0);
  ASSERT_TRUE(observed.receipt);
  EXPECT_EQ(observed.receipt->sequence, 1u);
  const auto json = stream_stats::client_media_interval_json(stream_stats::get_current());
  EXPECT_EQ(json.at("ingest_state"), "observed");
  EXPECT_EQ(json.at("interval").at("frames").at("lost"), 4);
  // The malformed section is this scope's newest receipt: unavailable, never evidence.
  EXPECT_EQ(json.at("frame_submission_receipt").at("reason"), "malformed_native_epoch");
  EXPECT_FALSE(json.at("frame_submission_receipt").at("available").get<bool>());
}
