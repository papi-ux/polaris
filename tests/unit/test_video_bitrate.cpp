/** @file tests/unit/test_video_bitrate.cpp
 * Real actuator transactions through the helper used by the parallel encode loop.
 * Backend calls are doubles; no GPU, capture, packet delivery or wall-clock timing is asserted.
 */
#include "src/video_bitrate.h"
#include "src/config.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
  struct backend_t {
    // Matches encode_session_t's return contract, including its trace values.
    enum class bitrate_update_e { rejected, applied, recreate_session };
    bitrate_update_e answer = bitrate_update_e::applied;
    int calls = 0;
    int target = 0;
    bitrate_update_e update_bitrate(int kbps) {
      ++calls;
      target = kbps;
      return answer;
    }
  };

#ifdef POLARIS_EXTRACTED_BASELINE
  // The retained local RED build supplies the verbatim c008 transaction and containing guard.
  // This adapter is never part of the registered native test target.
  #include "baseline_after_encode.h"
#endif

  class VideoBitrateBoundary {
  public:
    VideoBitrateBoundary() {
      config::video.adaptive_bitrate.enabled = true;
      config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
      config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
      adaptive_bitrate::set_base_bitrate(20000);
      adaptive_bitrate::set_runtime_update_supported(true, {}, 20000, true, 1);
    }
    ~VideoBitrateBoundary() {
      adaptive_bitrate::set_session_scope(0, "", false);
      adaptive_bitrate::set_runtime_update_supported(false);
      adaptive_bitrate::reset();
    }
    video::bitrate::result_t transaction(
        std::optional<adaptive_bitrate::live_bitrate_request_t> request, std::int64_t next_frame) {
      return video::bitrate::apply_after_encode(request, backend, applied, next_frame, true,
        [&](const auto &before, const auto &requested, int encoder_before,
            const char *result, std::optional<int> backend_result) {
          trace_before = before;
          trace_request = requested;
          traced_encoder_before = encoder_before;
          trace_result = result;
          traced_backend = backend_result;
        });
    }
    void after_encode(int next_frame) {
#ifdef POLARIS_EXTRACTED_BASELINE
      baseline_after_encode(next_frame, backend, applied);
#else
      last = transaction(adaptive_bitrate::get_live_bitrate_request(), next_frame);
#endif
    }
    backend_t backend;
    int applied = 20000;
    video::bitrate::result_t last;
    vrr_trace::bitrate_observation_t trace_before;
    adaptive_bitrate::live_bitrate_request_t trace_request {};
    int traced_encoder_before = 0;
    std::string trace_result;
    std::optional<int> traced_backend;
  };
}

TEST(AdaptiveBitrateController, EncodedBoundaryAppliesAndAcknowledgesAtAnArbitraryNonStatsBoundary) {
  VideoBitrateBoundary harness;
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_FALSE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 14000));
  harness.after_encode(17);  // Frame 16 just encoded; frame 17 must use the new rate.
  EXPECT_EQ(harness.backend.calls, 1);
  EXPECT_EQ(harness.backend.target, 14000);
  EXPECT_EQ(harness.applied, 14000);
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 14000));
}

TEST(AdaptiveBitrateController, EncodedBoundaryNoRequestAndUnchangedRateNeverCallTheBackend) {
  VideoBitrateBoundary harness;
  adaptive_bitrate::set_runtime_enabled(false);
  ASSERT_FALSE(adaptive_bitrate::get_live_bitrate_request());
  harness.after_encode(19);
  EXPECT_EQ(harness.backend.calls, 0);
  adaptive_bitrate::set_live_bitrate(20000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  // Make proof unknown without changing the encoder-thread rate. Polling alone cannot restore it;
  // this requires the same-rate ACK branch, still without a backend reconfiguration.
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(request->revision, 20000));
  ASSERT_FALSE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 20000));
  harness.after_encode(20);
  harness.after_encode(21);
  EXPECT_EQ(harness.backend.calls, 0);
  EXPECT_EQ(harness.applied, 20000);
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 20000));
}

TEST(AdaptiveBitrateController, EncodedBoundarySupersededRequestCannotCallTheBackendOrAcknowledge) {
  VideoBitrateBoundary harness;
  adaptive_bitrate::set_live_bitrate(14000);
  const auto stale = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(stale);
  adaptive_bitrate::set_live_bitrate(16000);
  EXPECT_EQ(harness.transaction(stale, 23).result, video::bitrate::result_e::superseded);
  EXPECT_EQ(harness.backend.calls, 0);
  EXPECT_EQ(harness.applied, 20000);
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(stale->revision, 14000));
  EXPECT_EQ(harness.trace_result, "superseded");
  EXPECT_FALSE(harness.traced_backend);
  EXPECT_EQ(harness.trace_before.requested_before_kbps, 16000);
  harness.after_encode(24);
  EXPECT_EQ(harness.applied, 16000);
  EXPECT_EQ(harness.backend.calls, 1);
}

TEST(AdaptiveBitrateController, EncodedBoundaryRejectionDisablesRuntimeUpdatesWithoutAnAppliedAck) {
  VideoBitrateBoundary harness;
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  harness.backend.answer = backend_t::bitrate_update_e::rejected;
  harness.after_encode(26);
  EXPECT_EQ(harness.last.result, video::bitrate::result_e::rejected);
  EXPECT_EQ(harness.applied, 20000);
  EXPECT_EQ(harness.backend.calls, 1);
  EXPECT_FALSE(adaptive_bitrate::get_state().runtime_update_supported);
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 14000));
  EXPECT_EQ(harness.trace_result, "rejected");
  EXPECT_EQ(harness.traced_encoder_before, 20000);
  harness.after_encode(27);
  EXPECT_EQ(harness.backend.calls, 1);
}

TEST(AdaptiveBitrateController, EncodedBoundaryRecreationInvalidatesProofUntilTheReplacementOpens) {
  VideoBitrateBoundary harness;
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  harness.backend.answer = backend_t::bitrate_update_e::recreate_session;
  harness.after_encode(29);
  EXPECT_EQ(harness.last.result, video::bitrate::result_e::recreate_session);
  EXPECT_EQ(harness.last.target_bitrate_kbps, 14000);
  EXPECT_EQ(harness.applied, 20000);
  EXPECT_EQ(harness.backend.calls, 1);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 0);
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 14000));
  EXPECT_EQ(harness.trace_result, "recreate");
  adaptive_bitrate::set_runtime_update_supported(true, {}, 14000);
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(request->revision, 14000));
}

TEST(AdaptiveBitrateController, EncodedBoundaryNativeOpenBeforeANewerTargetIsReadyAtItsAcceptedRate) {
  struct case_t {
    const char *name;
    int newer_kbps;
    backend_t::bitrate_update_e answer;
  };
  for (const auto &c : {case_t {"same target", 16000, backend_t::bitrate_update_e::applied},
                        case_t {"newer target applied", 14000, backend_t::bitrate_update_e::applied},
                        case_t {"newer target refused", 14000, backend_t::bitrate_update_e::rejected}}) {
    SCOPED_TRACE(c.name);
    // An FFmpeg session asks for a rebuild at 16000. Runtime support stays published across it, so
    // the rebuild's poll and encode_run's own poll both return live requests.
    VideoBitrateBoundary harness;
    adaptive_bitrate::set_live_bitrate(16000);
    harness.backend.answer = backend_t::bitrate_update_e::recreate_session;
    harness.after_encode(1);
    ASSERT_EQ(harness.last.result, video::bitrate::result_e::recreate_session);
    harness.backend = {};

    // The native encoder opens at the rebuild poll's target and its SDK accepts 16000...
    const auto opened = adaptive_bitrate::get_live_bitrate_request();
    ASSERT_TRUE(opened);
    const std::optional<int> accepted = opened->target_bitrate_kbps;
    ASSERT_EQ(accepted, 16000);
    // ...then an explicit writer moves the target before encode_run's own poll.
    adaptive_bitrate::set_live_bitrate(c.newer_kbps);
    const auto newer = adaptive_bitrate::get_live_bitrate_request();
    ASSERT_TRUE(newer);
    ASSERT_EQ(newer->target_bitrate_kbps, c.newer_kbps);

    // Readiness and the loop's applied rate start from the open, not the request it never received.
    const int ready = video::bitrate::opened_bitrate_kbps(accepted, newer->target_bitrate_kbps);
    adaptive_bitrate::set_runtime_update_supported(true, {}, ready, true, 1);
    harness.applied = ready;
    EXPECT_EQ(ready, 16000);
    EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 16000);
    EXPECT_EQ(harness.backend.calls, 0);

    harness.backend.answer = c.answer;
    harness.after_encode(2);
    harness.after_encode(3);
    if (c.newer_kbps == 16000) {
      EXPECT_EQ(harness.backend.calls, 0);
      EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(newer->revision, 16000));
    } else if (c.answer == backend_t::bitrate_update_e::applied) {
      // One real update, acknowledged only by its success; the next poll is a same-rate ACK.
      EXPECT_EQ(harness.backend.calls, 1);
      EXPECT_EQ(harness.backend.target, 14000);
      EXPECT_EQ(harness.applied, 14000);
      EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(newer->revision, 14000));
    } else {
      // The existing failure semantics: no ACK, and runtime updates stop for this session.
      EXPECT_EQ(harness.backend.calls, 1);
      EXPECT_EQ(harness.applied, 16000);
      EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(newer->revision, 14000));
      EXPECT_FALSE(adaptive_bitrate::get_state().runtime_update_supported);
    }
  }

  // A session without its own accepted rate (FFmpeg opens after the poll) keeps the configured one.
  EXPECT_EQ(video::bitrate::opened_bitrate_kbps(std::nullopt, 14000), 14000);
  EXPECT_EQ(video::bitrate::opened_bitrate_kbps(0, 14000), 14000);
}

TEST(AdaptiveBitrateController, EncodedBoundaryAppliedTraceKeepsThePreUpdateEncoderAndRevision) {
  VideoBitrateBoundary harness;
  adaptive_bitrate::set_live_bitrate(14000);
  harness.after_encode(31);
  EXPECT_EQ(harness.trace_result, "applied");
  EXPECT_EQ(harness.traced_backend, static_cast<int>(backend_t::bitrate_update_e::applied));
  EXPECT_EQ(harness.traced_encoder_before, 20000);
  EXPECT_EQ(harness.trace_before.applied_before_kbps, 20000);
  EXPECT_EQ(harness.trace_request.target_bitrate_kbps, 14000);
  EXPECT_EQ(harness.trace_request.revision, harness.trace_before.requested_before_revision);
}

TEST(AdaptiveBitrateController, EncodedBoundaryProductionPollAndTransactionFollowEncodeOutsideTheStatsGate) {
  std::ifstream input(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/video.cpp");
  ASSERT_TRUE(input);
  std::ostringstream text;
  text << input.rdbuf();
  const auto source = text.str();
  const auto loop = source.find("  void encode_run(");
  const auto encoded = source.find("if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp))", loop);
  const auto call = source.find("bitrate::apply_after_encode(", encoded);
  const auto stats_gate = source.find("if (frame_nr % 30 == 0)", encoded);
  ASSERT_NE(encoded, std::string::npos);
  ASSERT_NE(call, std::string::npos);
  ASSERT_NE(stats_gate, std::string::npos);
  EXPECT_LT(encoded, call);
  EXPECT_LT(call, stats_gate);
  const auto poll = source.find("adaptive_bitrate::get_live_bitrate_request()", call);
  EXPECT_LT(poll, stats_gate);
  EXPECT_NE(source.substr(call, stats_gate - call).find("applied_adaptive_bitrate, frame_nr,"), std::string::npos);
  const auto effective = source.find("const int effective_bitrate = applied_adaptive_bitrate;", stats_gate);
  const auto publish = source.find("stream_stats::update_video_stats(", stats_gate);
  ASSERT_NE(effective, std::string::npos);
  ASSERT_NE(publish, std::string::npos);
  EXPECT_LT(effective, publish);
}
