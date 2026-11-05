/**
 * @file tests/unit/test_stream_frame_feedback.cpp
 * @brief Frame-submission feedback (#611): sender ledger, send outcomes, parser and interval join.
 *
 * Every frame record comes from the real encoder-setting provenance ledger, every send outcome from
 * the real per-frame sender record, and every snapshot through the real wire parser.
 */
#include "src/stream_frame_feedback.h"
#include "src/stream_frame_feedback_json.h"
#include "src/video_bitrate_provenance.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
  namespace bp = video::bitrate_provenance;
  namespace ff = stream_frame_feedback;
  using clock = ff::clock;

  constexpr std::uint64_t k_generation = 611;
  constexpr auto k_bound = 5s;  // The existing client media coverage bound the owner passes in.
  const std::string k_nonce_a = "0123456789abcdef0123456789abcdef";
  const std::string k_nonce_b = "fedcba9876543210fedcba9876543210";
  // The successful launch/resume attempt nonce the scope was admitted with; separate from the above.
  const std::string k_launch = "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a";
  const std::string k_other_launch = "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5";

  // One encoder lifetime, producing real immutable records through the production provenance ledger.
  struct encoder_t {
    explicit encoder_t(std::uint64_t lifetime, std::uint64_t generation = k_generation):
        scope {lifetime, generation}, ledger {scope} {}

    bool apply(int kbps, bp::receipt_origin_e origin = bp::receipt_origin_e::backend_reconfiguration) {
      ++application;
      return ledger.observe_receipt({scope, application, application * 10, kbps, origin});
    }
    bool open(int kbps) { return apply(kbps, bp::receipt_origin_e::successful_open); }

    // Encode one input: capture before the work, commit after it succeeded, take at the output.
    std::optional<const bp::record_t> encode(std::int64_t pts) {
      auto submission = ledger.capture(pts);
      if (!submission) return std::nullopt;
      EXPECT_TRUE(ledger.complete(*submission, bp::submission_result_e::submitted_under_captured_setting));
      auto record = ledger.take(scope, pts);
      if (!record) return std::nullopt;
      return std::optional<const bp::record_t> {*record};
    }

    bp::scope_t scope;
    bp::ledger_t ledger;
    std::uint64_t application = 0;
  };

  enum class path_e { sent, batch_false_fallback_ok, batch_false_single_failed, cancelled_after_batch,
                      cancelled_after_success, exception_after_batch, source_timestamp_unavailable,
                      abandoned, pending };

  // Drive the real per-frame sender record the way the broadcast loop does.
  void send(ff::sender_ledger_t &ledger, std::int64_t index, const std::optional<const bp::record_t> &setting,
            path_e path = path_e::sent, std::unique_ptr<ff::frame_send_record_t> *keep_pending = nullptr) {
    auto record = std::make_unique<ff::frame_send_record_t>(&ledger, index, setting);
    switch (path) {
      case path_e::sent:
        record->send_returned(true, true);
        record->send_returned(true, true);
        record->path_finished(false);
        break;
      case path_e::batch_false_fallback_ok:
        record->send_returned(true, false);
        for (int shard = 0; shard < 3; ++shard) record->send_returned(false, true);
        record->path_finished(false);
        break;
      case path_e::batch_false_single_failed:
        record->send_returned(true, false);
        record->send_returned(false, true);
        record->send_returned(false, false);
        record->path_finished(false);
        break;
      case path_e::cancelled_after_batch:
        record->send_returned(true, false);
        record->path_finished(true);
        break;
      case path_e::cancelled_after_success:
        record->send_returned(true, true);
        record->path_finished(true);
        break;
      case path_e::exception_after_batch:
        record->send_returned(true, true);
        record->exception();
        break;
      case path_e::source_timestamp_unavailable:
        record->not_sent();
        break;
      case path_e::abandoned:
        record->send_returned(true, true);
        break;
      case path_e::pending:
        record->send_returned(true, true);
        ASSERT_NE(keep_pending, nullptr);
        *keep_pending = std::move(record);
        return;
    }
  }

  struct snapshot_args_t {
    std::uint64_t epoch = 1;
    std::uint64_t sequence = 1;
    std::uint64_t cursor = 1;
    std::uint64_t units = 1;
    std::uint64_t bytes = 1000;
    std::uint64_t at_ms = 10'000;
    std::uint64_t last_submission_ms = 10'000;
    std::string nonce = k_nonce_a;
    std::string launch = k_launch;
  };

  nlohmann::json sample_with(const snapshot_args_t &a) {
    return {
      {"monotonic_timestamp_ms", 123}, {"frames_expected", 10}, {"frames_received", 10}, {"frames_lost", 0},
      {"frame_submission", {
        {"version", 1}, {"launch_nonce", a.launch}, {"connection_nonce", a.nonce}, {"native_epoch", a.epoch},
        {"snapshot_sequence", a.sequence}, {"last_frame_index", a.cursor},
        {"decode_units_submitted_total", a.units}, {"payload_bytes_total", a.bytes},
        {"monotonic_timestamp_ms", a.at_ms}, {"last_submission_timestamp_ms", a.last_submission_ms},
      }},
    };
  }

  ff::section_t parse(const snapshot_args_t &a) {
    const auto section = ff::parse_section(sample_with(a));
    EXPECT_EQ(section.state, ff::section_t::state_e::valid) << section.reason;
    return section;
  }

  struct scope_fixture_t {
    std::shared_ptr<ff::sender_ledger_t> ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
    ff::scope_feedback_t scope {ledger, k_bound, k_launch};

    ff::receipt_t ingest(const snapshot_args_t &a, clock::time_point at = clock::now()) {
      return scope.ingest(parse(a), at);
    }
  };

  std::string reason(const ff::receipt_t &receipt) { return receipt.reason; }
}  // namespace

// ---------------------------------------------------------------------------------------------------
// Sender ledger and the join of every frame in (p, q]

TEST(StreamFrameFeedbackJoin, InitialSuccessfulOpenFramesJoinUnderThatExactSetting) {
  encoder_t encoder {7};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 10; ++frame) send(*f.ledger, frame, encoder.encode(frame));

  const auto baseline = f.ingest({.sequence = 1, .cursor = 4, .units = 4, .bytes = 4000, .at_ms = 1000, .last_submission_ms = 990});
  EXPECT_FALSE(baseline.available);
  EXPECT_EQ(reason(baseline), "baseline");

  const auto joined = f.ingest({.sequence = 2, .cursor = 10, .units = 9, .bytes = 9000, .at_ms = 1100, .last_submission_ms = 1095});
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(reason(joined), "joined");
  EXPECT_EQ(joined.from_frame_exclusive, 4u);
  EXPECT_EQ(joined.to_frame_inclusive, 10u);
  EXPECT_EQ(joined.frames_emitted, 6u);
  EXPECT_EQ(joined.decode_units_submitted, 5u);
  EXPECT_EQ(joined.frames_absent_at_submission, 1u);  // Absent at decoder submission, not network loss.
  EXPECT_EQ(joined.payload_bytes, 5000u);
  EXPECT_EQ(joined.client_interval_ms, 100u);
  EXPECT_EQ(joined.encoder_setting.encoder_lifetime, 7u);
  EXPECT_EQ(joined.encoder_setting.stream_generation, k_generation);
  EXPECT_EQ(joined.encoder_setting.application_sequence, 1u);
  EXPECT_EQ(joined.encoder_setting.origin_revision, 10u);
  EXPECT_EQ(joined.encoder_setting.configured_encoder_kbps, 20'000);
  EXPECT_EQ(joined.encoder_setting.receipt_origin, bp::receipt_origin_e::successful_open);
}

TEST(StreamFrameFeedbackJoin, OldQueuedAFramesSentAfterBIsAppliedStayAttributedToA) {
  encoder_t encoder {8};
  ASSERT_TRUE(encoder.open(20'000));
  std::vector<std::optional<const bp::record_t>> queued;
  for (int frame = 1; frame <= 6; ++frame) queued.push_back(encoder.encode(frame));
  ASSERT_TRUE(encoder.apply(12'000));  // B applied while A frames are still queued for the sender.
  for (int frame = 7; frame <= 12; ++frame) queued.push_back(encoder.encode(frame));

  // The A frames reach the sender only now, after B became the current setting.
  scope_fixture_t f;
  for (int frame = 1; frame <= 12; ++frame) send(*f.ledger, frame, queued[frame - 1]);

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto old_a = f.ingest({.sequence = 2, .cursor = 6, .units = 6, .bytes = 6, .at_ms = 200, .last_submission_ms = 200});
  ASSERT_TRUE(old_a.available) << old_a.reason;
  EXPECT_EQ(old_a.encoder_setting.configured_encoder_kbps, 20'000);
  EXPECT_EQ(old_a.encoder_setting.application_sequence, 1u);

  const auto b = f.ingest({.sequence = 3, .cursor = 12, .units = 12, .bytes = 12, .at_ms = 300, .last_submission_ms = 300});
  ASSERT_TRUE(b.available) << b.reason;
  EXPECT_EQ(b.encoder_setting.configured_encoder_kbps, 12'000);
  EXPECT_EQ(b.encoder_setting.application_sequence, 2u);
}

TEST(StreamFrameFeedbackJoin, ARangeCrossingAToBIsMixedAndALaterBOnlyRangeRecovers) {
  encoder_t encoder {9};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 5; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  ASSERT_TRUE(encoder.apply(14'000));
  for (int frame = 6; frame <= 15; ++frame) send(*f.ledger, frame, encoder.encode(frame));

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 3, .units = 3, .bytes = 3, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto mixed = f.ingest({.sequence = 2, .cursor = 8, .units = 8, .bytes = 8, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_FALSE(mixed.available);
  EXPECT_EQ(reason(mixed), "mixed_encoder_settings");
  ASSERT_TRUE(mixed.unavailable_frame_index);
  EXPECT_EQ(*mixed.unavailable_frame_index, 6u);
  // The mixed snapshot was still consumed as the next baseline, so B-only frames join afterwards.
  const auto b_only = f.ingest({.sequence = 3, .cursor = 15, .units = 15, .bytes = 15, .at_ms = 300, .last_submission_ms = 300});
  ASSERT_TRUE(b_only.available) << b_only.reason;
  EXPECT_EQ(b_only.from_frame_exclusive, 8u);
  EXPECT_EQ(b_only.encoder_setting.configured_encoder_kbps, 14'000);
}

TEST(StreamFrameFeedbackJoin, AToBToAAtTheSameKbpsIsMixedEvenWithMatchingEndpoints) {
  encoder_t encoder {10};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 3; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  ASSERT_TRUE(encoder.apply(15'000));
  for (int frame = 4; frame <= 5; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  ASSERT_TRUE(encoder.apply(20'000));  // A again: same kbps, a different application.
  for (int frame = 6; frame <= 8; ++frame) send(*f.ledger, frame, encoder.encode(frame));

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 1, .units = 1, .bytes = 1, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto aba = f.ingest({.sequence = 2, .cursor = 8, .units = 7, .bytes = 7, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_FALSE(aba.available);
  EXPECT_EQ(reason(aba), "mixed_encoder_settings");
}

TEST(StreamFrameFeedbackJoin, EqualKbpsApplicationsWithNoFramesBetweenThemAreStillMixed) {
  encoder_t encoder {11};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 3; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  ASSERT_TRUE(encoder.apply(10'000));  // B applied but encoded nothing.
  ASSERT_TRUE(encoder.apply(20'000));  // A' at A's kbps.
  for (int frame = 4; frame <= 6; ++frame) send(*f.ledger, frame, encoder.encode(frame));

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 1, .units = 1, .bytes = 1, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto receipt = f.ingest({.sequence = 2, .cursor = 6, .units = 5, .bytes = 5, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_FALSE(receipt.available);
  EXPECT_EQ(reason(receipt), "mixed_encoder_settings");
  ASSERT_TRUE(receipt.unavailable_frame_index);
  EXPECT_EQ(*receipt.unavailable_frame_index, 4u);
}

TEST(StreamFrameFeedbackJoin, EveryFrameInTheRangeIsInspectedNotOnlyItsEndpoints) {
  encoder_t a {12};
  ASSERT_TRUE(a.open(20'000));
  encoder_t other_lifetime {13};
  ASSERT_TRUE(other_lifetime.open(20'000));  // Same kbps and origin, another encoder lifetime.
  scope_fixture_t f;
  for (int frame = 1; frame <= 9; ++frame) {
    send(*f.ledger, frame, frame == 5 ? other_lifetime.encode(frame) : a.encode(frame));
  }
  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 1, .units = 1, .bytes = 1, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto receipt = f.ingest({.sequence = 2, .cursor = 9, .units = 8, .bytes = 8, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_EQ(reason(receipt), "mixed_encoder_settings");
  EXPECT_EQ(receipt.unavailable_frame_index.value_or(0), 5u);
}

TEST(StreamFrameFeedbackJoin, MissingMetadataAndAnotherGenerationAreUnavailable) {
  encoder_t encoder {14};
  ASSERT_TRUE(encoder.open(20'000));
  encoder_t foreign {15, k_generation + 1};
  ASSERT_TRUE(foreign.open(20'000));
  scope_fixture_t f;
  send(*f.ledger, 1, encoder.encode(1));
  send(*f.ledger, 2, encoder.encode(2));
  send(*f.ledger, 3, std::nullopt);  // No immutable record reached this packet.
  send(*f.ledger, 4, encoder.encode(4));
  send(*f.ledger, 5, foreign.encode(5));

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto missing = f.ingest({.sequence = 2, .cursor = 4, .units = 4, .bytes = 4, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_EQ(reason(missing), "encoder_setting_missing");
  EXPECT_EQ(missing.unavailable_frame_index.value_or(0), 3u);
  const auto foreign_generation = f.ingest({.sequence = 3, .cursor = 5, .units = 5, .bytes = 5, .at_ms = 300, .last_submission_ms = 300});
  EXPECT_EQ(reason(foreign_generation), "encoder_setting_generation_mismatch");
}

TEST(StreamFrameFeedbackJoin, HolesFromFramesTheSenderNeverReachedAreUnavailable) {
  encoder_t encoder {16};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 10; ++frame) {
    auto record = encoder.encode(frame);
    if (frame != 6) send(*f.ledger, frame, record);  // Frame 6 was dropped before the sender.
  }
  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  const auto receipt = f.ingest({.sequence = 2, .cursor = 10, .units = 7, .bytes = 7, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_EQ(reason(receipt), "host_frame_missing");
  EXPECT_EQ(receipt.unavailable_frame_index.value_or(0), 6u);
}

TEST(StreamFrameFeedbackJoin, EvictedAndOversizedRangesAreUnavailableAndRetentionStaysBounded) {
  encoder_t encoder {17};
  ASSERT_TRUE(encoder.open(20'000));
  const auto total = static_cast<std::int64_t>(ff::k_ledger_capacity) + 100;
  scope_fixture_t f;
  for (std::int64_t frame = 1; frame <= total; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  const auto now = clock::now();
  // Frame 50 was overwritten by frame 50 + capacity.
  const auto evicted = f.ledger->join(49, 60, now, k_bound);
  EXPECT_FALSE(evicted.available);
  EXPECT_EQ(std::string {evicted.reason}, "ledger_evicted");
  EXPECT_EQ(evicted.frame_index.value_or(0), 50u);
  const auto oversized = f.ledger->join(10, static_cast<std::uint32_t>(total), now, k_bound);
  EXPECT_EQ(std::string {oversized.reason}, "range_exceeds_ledger");
  // The newest capacity's worth of frames is still retained.
  const auto retained = f.ledger->join(static_cast<std::uint32_t>(total - ff::k_ledger_capacity),
                                       static_cast<std::uint32_t>(total), now, k_bound);
  EXPECT_TRUE(retained.available) << retained.reason;
}

TEST(StreamFrameFeedbackJoin, ActualSendOutcomesOtherThanCompleteSuccessAreUnavailable) {
  struct case_t {
    path_e path;
    const char *reason;
  };
  const case_t cases[] = {
    {path_e::batch_false_fallback_ok, "send_batch_fallback"},  // Even when every fallback send succeeded.
    {path_e::batch_false_single_failed, "send_failed"},
    {path_e::cancelled_after_batch, "send_cancelled"},
    {path_e::cancelled_after_success, "send_cancelled"},
    {path_e::exception_after_batch, "send_exception"},
    {path_e::source_timestamp_unavailable, "frame_not_sent"},
    {path_e::abandoned, "send_incomplete"},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(c.reason);
    encoder_t encoder {18};
    ASSERT_TRUE(encoder.open(20'000));
    scope_fixture_t f;
    send(*f.ledger, 1, encoder.encode(1));
    send(*f.ledger, 2, encoder.encode(2));
    send(*f.ledger, 3, encoder.encode(3), c.path);
    send(*f.ledger, 4, encoder.encode(4));
    ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 1, .units = 1, .bytes = 1, .at_ms = 100, .last_submission_ms = 100})), "baseline");
    const auto receipt = f.ingest({.sequence = 2, .cursor = 4, .units = 3, .bytes = 3, .at_ms = 200, .last_submission_ms = 200});
    EXPECT_FALSE(receipt.available);
    EXPECT_EQ(reason(receipt), c.reason);
    EXPECT_EQ(receipt.unavailable_frame_index.value_or(0), 3u);
  }
}

TEST(StreamFrameFeedbackJoin, SendRecordDispositionFollowsOnlyActualReturnValues) {
  auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
  {
    ff::frame_send_record_t record {ledger.get(), 1, std::nullopt};
    EXPECT_EQ(record.disposition(), ff::send_disposition_e::incomplete);
    record.send_returned(true, true);
    record.path_finished(false);
    EXPECT_EQ(record.disposition(), ff::send_disposition_e::sent);
  }
  {
    // The loop always "returns" after a fallback; only the batch's own false is evidence.
    ff::frame_send_record_t record {ledger.get(), 2, std::nullopt};
    record.send_returned(true, false);
    record.send_returned(false, true);
    record.path_finished(false);
    EXPECT_EQ(record.disposition(), ff::send_disposition_e::batch_fallback);
  }
  {
    ff::frame_send_record_t record {ledger.get(), 3, std::nullopt};
    record.path_finished(false);
    EXPECT_EQ(record.disposition(), ff::send_disposition_e::not_sent);
  }
  {
    ff::frame_send_record_t no_ledger {nullptr, 4, std::nullopt};  // Ledger unavailable: records nothing.
    no_ledger.send_returned(true, true);
    no_ledger.path_finished(false);
  }
  EXPECT_EQ(ledger->high_water().value_or(0), 3);
}

TEST(StreamFrameFeedbackJoin, PendingSendIsDistinctFromAFutureCursorAndNeverJoins) {
  encoder_t encoder {19};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 4; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  std::unique_ptr<ff::frame_send_record_t> in_flight;
  send(*f.ledger, 5, encoder.encode(5), path_e::pending, &in_flight);

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  // The client already submitted frame 5 while the host's path for it has not finished.
  const auto pending = f.ingest({.sequence = 2, .cursor = 5, .units = 3, .bytes = 3, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_EQ(reason(pending), "send_pending");
  // A cursor the sender never began is refused and does not advance the baseline.
  const auto future = f.ingest({.sequence = 3, .cursor = 9, .units = 7, .bytes = 7, .at_ms = 300, .last_submission_ms = 300});
  EXPECT_EQ(reason(future), "cursor_beyond_sender_high_water");

  in_flight->path_finished(false);
  in_flight.reset();
  for (int frame = 6; frame <= 9; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  // Measured from the pending snapshot (cursor 5), the consumed baseline, not from the refused one.
  const auto later = f.ingest({.sequence = 4, .cursor = 9, .units = 7, .bytes = 7, .at_ms = 400, .last_submission_ms = 400});
  ASSERT_TRUE(later.available) << later.reason;
  EXPECT_EQ(later.from_frame_exclusive, 5u);
  EXPECT_EQ(later.decode_units_submitted, 4u);
}

TEST(StreamFrameFeedbackJoin, UnrepresentableOrNonIncreasingFrameIndexesNeverUnwrap) {
  encoder_t encoder {20};
  ASSERT_TRUE(encoder.open(20'000));
  {
    auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
    send(*ledger, 1, encoder.encode(1));
    send(*ledger, 2, encoder.encode(2));
    // Beyond the 32-bit wire field: it would alias frame 1 on the wire.
    send(*ledger, static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 2, encoder.encode(3));
    EXPECT_EQ(std::string {ledger->join(0, 2, clock::now(), k_bound).reason}, "frame_index_unrepresentable");
  }
  {
    auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
    send(*ledger, 5, encoder.encode(5));
    send(*ledger, 6, encoder.encode(6));
    send(*ledger, 6, encoder.encode(7));  // A reused index.
    EXPECT_EQ(std::string {ledger->join(5, 6, clock::now(), k_bound).reason}, "frame_index_not_increasing");
    EXPECT_EQ(ledger->high_water().value_or(0), 6);
  }
  {
    auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
    send(*ledger, -1, encoder.encode(8));
    EXPECT_EQ(std::string {ledger->join(0, 1, clock::now(), k_bound).reason}, "frame_index_unrepresentable");
  }
}

TEST(StreamFrameFeedbackJoin, ASendThatEnteredBeforeTheBoundIsStaleEvenWhenItsReturnIsFresh) {
  encoder_t encoder {23};
  ASSERT_TRUE(encoder.open(20'000));
  auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
  const auto now = clock::now();
  for (int frame = 1; frame <= 2; ++frame) {
    const auto ticket = ledger->begin(frame, ff::encoder_setting_of(encoder.encode(frame)), now - 100ms);
    ASSERT_TRUE(ticket);
    ledger->complete(*ticket, ff::send_disposition_e::sent, now - 50ms);
  }
  // Frame 3 entered its first send 6 s ago and a blocking call returned only now.
  const auto blocked = ledger->begin(3, ff::encoder_setting_of(encoder.encode(3)), now - 6s);
  ASSERT_TRUE(blocked);
  ledger->complete(*blocked, ff::send_disposition_e::sent, now - 10ms);
  EXPECT_TRUE(ledger->join(0, 2, now, k_bound).available);
  const auto join = ledger->join(0, 3, now, k_bound);
  EXPECT_FALSE(join.available);
  EXPECT_EQ(std::string {join.reason}, "host_send_stale");
  EXPECT_EQ(join.frame_index.value_or(0), 3u);
}

TEST(StreamFrameFeedbackJoin, StaleHostSendBoundariesAreUnavailable) {
  encoder_t encoder {21};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 6; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  const auto later = clock::now() + 6s;  // The sends ended over the bound before this report arrived.
  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100}, later - 1s)), "baseline");
  const auto receipt = f.ingest({.sequence = 2, .cursor = 6, .units = 4, .bytes = 4, .at_ms = 200, .last_submission_ms = 200}, later);
  EXPECT_EQ(reason(receipt), "host_send_stale");
}

TEST(StreamFrameFeedbackJoin, TheSenderLedgerWorksWithNoTraceInvolved) {
  // The ledger, record and join read no trace predicate or label: a bare ledger yields evidence.
  encoder_t encoder {22};
  ASSERT_TRUE(encoder.open(20'000));
  auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
  for (int frame = 1; frame <= 3; ++frame) send(*ledger, frame, encoder.encode(frame));
  const auto join = ledger->join(0, 3, clock::now(), k_bound);
  EXPECT_TRUE(join.available) << join.reason;
}

// ---------------------------------------------------------------------------------------------------
// Native snapshot epochs, ordering, coherence and freshness

TEST(StreamFrameFeedbackIngest, ACounterEpochResetWithGrowingTotalsIsABaselineOnly) {
  encoder_t encoder {30};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 40; ++frame) send(*f.ledger, frame, encoder.encode(frame));

  ASSERT_EQ(reason(f.ingest({.epoch = 1, .sequence = 1, .cursor = 5, .units = 5, .bytes = 500, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  // A fresh native setup whose totals exceed the old ones. Subtracting across it would invent media.
  const auto reset = f.ingest({.epoch = 2, .sequence = 1, .cursor = 20, .units = 18, .bytes = 90'000, .at_ms = 50, .last_submission_ms = 50, .nonce = k_nonce_b});
  EXPECT_FALSE(reset.available);
  EXPECT_EQ(reason(reset), "native_epoch_baseline");
  const auto next = f.ingest({.epoch = 2, .sequence = 2, .cursor = 30, .units = 26, .bytes = 90'800, .at_ms = 150, .last_submission_ms = 150, .nonce = k_nonce_b});
  ASSERT_TRUE(next.available) << next.reason;
  EXPECT_EQ(next.from_frame_exclusive, 20u);
  EXPECT_EQ(next.decode_units_submitted, 8u);
  EXPECT_EQ(next.payload_bytes, 800u);
}

TEST(StreamFrameFeedbackIngest, LowerEpochsAndSameEpochNonceMismatchNeverReplaceTheNewerBaseline) {
  encoder_t encoder {31};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 30; ++frame) send(*f.ledger, frame, encoder.encode(frame));

  ASSERT_EQ(reason(f.ingest({.epoch = 3, .sequence = 7, .cursor = 10, .units = 10, .bytes = 10, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  EXPECT_EQ(reason(f.ingest({.epoch = 2, .sequence = 99, .cursor = 25, .units = 25, .bytes = 25, .at_ms = 900, .last_submission_ms = 900, .nonce = k_nonce_b})),
            "native_epoch_regressed");
  EXPECT_EQ(reason(f.ingest({.epoch = 3, .sequence = 8, .cursor = 25, .units = 25, .bytes = 25, .at_ms = 200, .last_submission_ms = 200, .nonce = k_nonce_b})),
            "connection_nonce_mismatch");
  // A higher epoch reusing the old connection's nonce is no fresh native setup.
  EXPECT_EQ(reason(f.ingest({.epoch = 4, .sequence = 1, .cursor = 25, .units = 1, .bytes = 1, .at_ms = 200, .last_submission_ms = 200})),
            "connection_nonce_reused");
  // The epoch-3 baseline at cursor 10 is still the one this interval is measured from.
  const auto joined = f.ingest({.epoch = 3, .sequence = 9, .cursor = 20, .units = 20, .bytes = 20, .at_ms = 300, .last_submission_ms = 300});
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.from_frame_exclusive, 10u);
  EXPECT_EQ(joined.snapshot_sequence, 9u);
}

TEST(StreamFrameFeedbackIngest, ReplayedRegressingAndIncoherentSnapshotsAreRefusedWithoutReplacingTheBaseline) {
  encoder_t encoder {32};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 30; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  const snapshot_args_t base {.sequence = 5, .cursor = 10, .units = 8, .bytes = 1000, .at_ms = 1000, .last_submission_ms = 1000};
  ASSERT_EQ(reason(f.ingest(base)), "baseline");

  struct case_t {
    snapshot_args_t args;
    const char *reason;
  };
  const case_t cases[] = {
    {{.sequence = 5, .cursor = 12, .units = 10, .bytes = 1200, .at_ms = 1100, .last_submission_ms = 1100}, "snapshot_sequence_not_increasing"},
    {{.sequence = 4, .cursor = 12, .units = 10, .bytes = 1200, .at_ms = 1100, .last_submission_ms = 1100}, "snapshot_sequence_not_increasing"},
    {{.sequence = 6, .cursor = 12, .units = 10, .bytes = 1200, .at_ms = 1000, .last_submission_ms = 1000}, "receiver_clock_not_increasing"},
    {{.sequence = 6, .cursor = 9, .units = 8, .bytes = 1000, .at_ms = 1100, .last_submission_ms = 1000}, "counter_regressed"},
    {{.sequence = 6, .cursor = 12, .units = 7, .bytes = 1200, .at_ms = 1100, .last_submission_ms = 1100}, "counter_regressed"},
    {{.sequence = 6, .cursor = 12, .units = 10, .bytes = 999, .at_ms = 1100, .last_submission_ms = 1100}, "counter_regressed"},
    {{.sequence = 6, .cursor = 12, .units = 10, .bytes = 1200, .at_ms = 1100, .last_submission_ms = 999}, "counter_regressed"},
    // q == p: no unit and no byte may appear.
    {{.sequence = 6, .cursor = 10, .units = 9, .bytes = 1100, .at_ms = 1100, .last_submission_ms = 1100}, "counters_incoherent"},
    {{.sequence = 6, .cursor = 10, .units = 8, .bytes = 1001, .at_ms = 1100, .last_submission_ms = 1000}, "counters_incoherent"},
    // q > p: 0 < units <= q - p and bytes >= units.
    {{.sequence = 6, .cursor = 12, .units = 8, .bytes = 1000, .at_ms = 1100, .last_submission_ms = 1100}, "counters_incoherent"},
    {{.sequence = 6, .cursor = 12, .units = 11, .bytes = 1300, .at_ms = 1100, .last_submission_ms = 1100}, "counters_incoherent"},
    {{.sequence = 6, .cursor = 12, .units = 10, .bytes = 1001, .at_ms = 1100, .last_submission_ms = 1100}, "counters_incoherent"},
  };
  for (const auto &c : cases) {
    const auto receipt = f.ingest(c.args);
    EXPECT_FALSE(receipt.available);
    EXPECT_EQ(reason(receipt), c.reason) << c.args.sequence << " " << c.args.cursor;
  }
  // Unchanged baseline: sequence 5 at cursor 10.
  const auto joined = f.ingest({.sequence = 6, .cursor = 12, .units = 10, .bytes = 1002, .at_ms = 1100, .last_submission_ms = 1100});
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.from_frame_exclusive, 10u);
  EXPECT_EQ(joined.payload_bytes, 2u);
}

TEST(StreamFrameFeedbackIngest, StaleIntervalsAreUnavailableButStillBecomeTheNextBaseline) {
  encoder_t encoder {33};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 40; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  // Host arrival times end just after the sends, so only the bound being tested can fail.
  const auto end = clock::now() + 10ms;

  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 5, .units = 5, .bytes = 5, .at_ms = 1000, .last_submission_ms = 500}, end - 5300ms)), "baseline");
  EXPECT_EQ(reason(f.ingest({.sequence = 2, .cursor = 10, .units = 10, .bytes = 10, .at_ms = 5600, .last_submission_ms = 500}, end - 5200ms)),
            "last_submission_stale");
  EXPECT_EQ(reason(f.ingest({.sequence = 3, .cursor = 15, .units = 15, .bytes = 15, .at_ms = 10'601, .last_submission_ms = 10'601}, end - 5100ms)),
            "receiver_interval_exceeds_bound");
  EXPECT_EQ(reason(f.ingest({.sequence = 4, .cursor = 20, .units = 20, .bytes = 20, .at_ms = 10'700, .last_submission_ms = 10'700}, end - 99ms)),
            "host_arrival_interval_exceeds_bound");
  // Each refused interval was still consumed, so the next one is measured from the snapshot just before
  // it. Host arrival and client clocks are never compared with each other, only each with itself.
  const auto joined = f.ingest({.sequence = 5, .cursor = 25, .units = 25, .bytes = 25, .at_ms = 10'799, .last_submission_ms = 10'790}, end);
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.from_frame_exclusive, 20u);
  EXPECT_EQ(joined.client_interval_ms, 99u);
}

TEST(StreamFrameFeedbackIngest, EmptySnapshotsAdvanceTheBaselineButNeverRefreshEvidenceAge) {
  encoder_t encoder {34};
  ASSERT_TRUE(encoder.open(20'000));
  for (const bool late : {false, true}) {
    SCOPED_TRACE(late);
    scope_fixture_t f;
    for (int frame = 1; frame <= 20; ++frame) send(*f.ledger, frame, encoder.encode(frame + (late ? 100 : 0)), path_e::sent);
    const auto t0 = clock::now();
    ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 10, .units = 10, .bytes = 10, .at_ms = 1000, .last_submission_ms = 1000}, t0)), "baseline");
    const auto empty = f.ingest({.sequence = 2, .cursor = 10, .units = 10, .bytes = 10, .at_ms = 4000, .last_submission_ms = 1000}, t0 + 3s);
    EXPECT_FALSE(empty.available);
    EXPECT_EQ(reason(empty), "no_frames_in_interval");
    // The empty snapshot's sequence is consumed: replaying it is refused.
    EXPECT_EQ(reason(f.ingest({.sequence = 2, .cursor = 12, .units = 12, .bytes = 12, .at_ms = 4100, .last_submission_ms = 4100}, t0 + 3100ms)),
              "snapshot_sequence_not_increasing");
    const auto at_ms = late ? 6500u : 4500u;
    const auto receipt = f.ingest({.sequence = 3, .cursor = 20, .units = 20, .bytes = 20, .at_ms = at_ms, .last_submission_ms = at_ms},
                                  t0 + (late ? 5500ms : 3500ms));
    if (late) {
      // Measured from the last snapshot that carried frames, not from the empty one.
      EXPECT_EQ(reason(receipt), "receiver_interval_exceeds_bound");
    } else {
      // Freshness was gated from the anchor (3500 ms); the metric is the native snapshot delta.
      ASSERT_TRUE(receipt.available) << receipt.reason;
      EXPECT_EQ(receipt.client_interval_ms, 500u);
      EXPECT_EQ(receipt.from_frame_exclusive, 10u);
      EXPECT_EQ(receipt.payload_bytes, 10u);
    }
  }
}

TEST(StreamFrameFeedbackIngest, FirstSnapshotBeyondTheSenderHighWaterIsNoBaseline) {
  encoder_t encoder {35};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  EXPECT_EQ(reason(f.ingest({.sequence = 1, .cursor = 3, .units = 3, .bytes = 3, .at_ms = 100, .last_submission_ms = 100})),
            "cursor_beyond_sender_high_water");
  for (int frame = 1; frame <= 6; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  // Nothing was consumed above, so this is the first baseline, and a higher epoch is refused too.
  EXPECT_EQ(reason(f.ingest({.sequence = 2, .cursor = 3, .units = 3, .bytes = 3, .at_ms = 200, .last_submission_ms = 200})), "baseline");
  EXPECT_EQ(reason(f.ingest({.epoch = 2, .sequence = 1, .cursor = 9, .units = 1, .bytes = 1, .at_ms = 1, .last_submission_ms = 1, .nonce = k_nonce_b})),
            "cursor_beyond_sender_high_water");
  const auto joined = f.ingest({.sequence = 3, .cursor = 6, .units = 6, .bytes = 6, .at_ms = 300, .last_submission_ms = 300});
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.native_epoch, 1u);
}

TEST(StreamFrameFeedbackIngest, MalformedSectionsAreUnavailableAndLeaveTheBaselineAlone) {
  encoder_t encoder {36};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 10; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 4, .units = 4, .bytes = 4, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  auto malformed = sample_with({.sequence = 2, .cursor = 10, .units = 10, .bytes = 10, .at_ms = 200, .last_submission_ms = 200});
  malformed["frame_submission"]["last_frame_index"] = 10.0;
  const auto receipt = f.scope.ingest(ff::parse_section(malformed), clock::now());
  EXPECT_FALSE(receipt.available);
  EXPECT_EQ(reason(receipt), "malformed_last_frame_index");
  EXPECT_FALSE(receipt.snapshot_echo);
  ASSERT_TRUE(f.scope.latest());
  EXPECT_EQ(std::string {f.scope.latest()->reason}, "malformed_last_frame_index");
  // The same snapshot, well formed, is measured from the unchanged baseline.
  const auto joined = f.ingest({.sequence = 2, .cursor = 10, .units = 10, .bytes = 10, .at_ms = 200, .last_submission_ms = 200});
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.from_frame_exclusive, 4u);
}

TEST(StreamFrameFeedbackIngest, RetirementEndsTheScopeAndItsLedger) {
  encoder_t encoder {37};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 6; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  ASSERT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100})), "baseline");
  f.scope.retire();
  const auto receipt = f.ingest({.sequence = 2, .cursor = 6, .units = 6, .bytes = 6, .at_ms = 200, .last_submission_ms = 200});
  EXPECT_FALSE(receipt.available);
  EXPECT_EQ(reason(receipt), "scope_retired");
  EXPECT_EQ(std::string {f.ledger->join(2, 6, clock::now(), k_bound).reason}, "scope_retired");
  // A late send from the retiring stream records nothing and cannot revive it.
  send(*f.ledger, 7, encoder.encode(7));
  EXPECT_EQ(std::string {f.ledger->join(6, 7, clock::now(), k_bound).reason}, "scope_retired");
}

TEST(StreamFrameFeedbackIngest, AScopeWithoutASenderLedgerNeverEstablishesEvidence) {
  ff::scope_feedback_t scope {nullptr, k_bound, k_launch};
  const auto receipt = scope.ingest(parse({}), clock::now());
  EXPECT_FALSE(receipt.available);
  EXPECT_EQ(reason(receipt), "sender_ledger_unavailable");
}

TEST(StreamFrameFeedbackIngest, OnlyTheExactLaunchNonceOfTheScopeBindsAndAMismatchKeepsTheBaseline) {
  encoder_t encoder {38};
  ASSERT_TRUE(encoder.open(20'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 9; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  // A report from another launch attempt (an earlier retry, or the launch a resume replaced).
  EXPECT_EQ(reason(f.ingest({.sequence = 1, .cursor = 2, .units = 2, .bytes = 2, .at_ms = 100, .last_submission_ms = 100,
                             .launch = k_other_launch})), "launch_nonce_mismatch");
  ASSERT_EQ(reason(f.ingest({.sequence = 2, .cursor = 4, .units = 4, .bytes = 4, .at_ms = 200, .last_submission_ms = 200})), "baseline");
  EXPECT_EQ(reason(f.ingest({.sequence = 3, .cursor = 9, .units = 9, .bytes = 9, .at_ms = 300, .last_submission_ms = 300,
                             .launch = k_other_launch})), "launch_nonce_mismatch");
  const auto joined = f.ingest({.sequence = 4, .cursor = 9, .units = 9, .bytes = 9, .at_ms = 400, .last_submission_ms = 400});
  ASSERT_TRUE(joined.available) << joined.reason;
  EXPECT_EQ(joined.from_frame_exclusive, 4u);

  // A scope admitted without a valid launch nonce never binds a report, whatever it carries.
  for (const auto &retained : {std::string {}, std::string {"5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A"},
                                     std::string(32, '0')}) {
    ff::scope_feedback_t unbound {f.ledger, k_bound, retained};
    EXPECT_EQ(reason(unbound.ingest(parse({}), clock::now())), "launch_nonce_unavailable");
  }
}

TEST(StreamFrameFeedbackIngest, ConcurrentSendsFeedbackAndRetirementOnlyJoinFullySentHomogeneousRanges) {
  encoder_t a {40};
  ASSERT_TRUE(a.open(20'000));
  encoder_t b {41};
  ASSERT_TRUE(b.open(12'000));
  // Settings alternate every 50 frames; every 17th frame's batch fails and falls back.
  constexpr int k_frames = 3000;
  constexpr int k_chunk = 10;
  const auto kbps_of = [](std::uint32_t frame) { return (frame / 50) % 2 ? 12'000 : 20'000; };
  std::vector<std::optional<const bp::record_t>> records;
  for (int frame = 1; frame <= k_frames; ++frame) records.push_back((kbps_of(frame) == 12'000 ? b : a).encode(frame));

  auto ledger = std::make_shared<ff::sender_ledger_t>(k_generation);
  auto scope = std::make_shared<ff::scope_feedback_t>(ledger, k_bound, k_launch);
  std::atomic<int> released {0}, completed {0};
  std::thread sender([&] {
    for (int frame = 1; frame <= k_frames; ++frame) {
      while (frame > released.load(std::memory_order_acquire)) std::this_thread::yield();
      {
        ff::frame_send_record_t record {ledger.get(), frame, records[frame - 1]};
        record.send_returned(true, frame % 17 != 0);
        if (frame % 17 == 0) record.send_returned(false, true);
        record.path_finished(false);
      }
      completed.store(frame, std::memory_order_release);
    }
  });

  std::uint64_t sequence = 0, at_ms = 1000;
  std::uint32_t consumed = 0;
  int joined = 0, racing = 0;
  const auto report = [&](std::uint32_t cursor) {
    const auto receipt = scope->ingest(parse({.sequence = ++sequence, .cursor = cursor, .units = cursor, .bytes = cursor,
                                              .at_ms = ++at_ms, .last_submission_ms = at_ms}), clock::now());
    const std::string why = receipt.reason;
    if (why != "cursor_beyond_sender_high_water") consumed = cursor;
    if (!receipt.available) return;
    ++joined;
    EXPECT_EQ(why, "joined");
    for (auto frame = receipt.from_frame_exclusive + 1; frame <= receipt.to_frame_inclusive; ++frame) {
      ASSERT_LE(frame, static_cast<std::uint32_t>(completed.load(std::memory_order_acquire))) << "an unfinished send joined";
      ASSERT_NE(frame % 17, 0u) << "a fallback frame joined";
      ASSERT_EQ(kbps_of(frame), receipt.encoder_setting.configured_encoder_kbps) << "a mixed range joined";
    }
  };
  for (int chunk_end = k_chunk; chunk_end <= k_frames; chunk_end += k_chunk) {
    released.store(chunk_end, std::memory_order_release);
    // Race the sender: report a frame it may still be sending, or one it has not begun.
    while (completed.load(std::memory_order_acquire) < chunk_end) {
      const auto racing_cursor = std::min<std::uint32_t>(completed.load(std::memory_order_acquire) + 1, chunk_end - 1);
      if (racing_cursor > consumed) {
        report(racing_cursor);
        ++racing;
      }
      std::this_thread::yield();
    }
    report(static_cast<std::uint32_t>(chunk_end));  // Every frame of this chunk has completed.
  }
  sender.join();
  // Chunks such as 21..30 contain no fallback frame and one setting, so at least those join.
  EXPECT_GT(joined, 0);
  ::testing::Test::RecordProperty("joined_intervals", joined);
  ::testing::Test::RecordProperty("racing_reports", racing);

  std::thread retirer([&] { scope->retire(); });
  const auto during = scope->ingest(parse({.sequence = ++sequence, .cursor = k_frames, .units = k_frames, .bytes = k_frames,
                                           .at_ms = ++at_ms, .last_submission_ms = at_ms}), clock::now());
  retirer.join();
  EXPECT_FALSE(during.available);
  EXPECT_EQ(reason(scope->ingest(parse({.sequence = ++sequence, .cursor = k_frames, .units = k_frames, .bytes = k_frames,
                                        .at_ms = ++at_ms, .last_submission_ms = at_ms}), clock::now())), "scope_retired");
}

// ---------------------------------------------------------------------------------------------------
// Wire parser and receipt

TEST(StreamFrameFeedbackParser, AbsentSectionsAreAbsentAndAValidOneParsesExactly) {
  EXPECT_EQ(ff::parse_section(nlohmann::json::object()).state, ff::section_t::state_e::absent);
  EXPECT_EQ(ff::parse_section(nlohmann::json::array()).state, ff::section_t::state_e::absent);
  const auto max64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  auto sample = sample_with({.epoch = max64, .sequence = max64, .cursor = std::numeric_limits<std::uint32_t>::max(),
                             .units = std::numeric_limits<std::uint32_t>::max(), .bytes = max64, .at_ms = max64,
                             .last_submission_ms = max64});
  const auto section = ff::parse_section(sample);
  ASSERT_EQ(section.state, ff::section_t::state_e::valid) << section.reason;
  EXPECT_EQ(section.snapshot.native_epoch, max64);
  EXPECT_EQ(section.snapshot.last_frame_index, std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(section.snapshot.connection_nonce, k_nonce_a);
  EXPECT_EQ(section.snapshot.launch_nonce, k_launch);
  // Parsed from a serialized body, as the route receives it.
  const auto reparsed = ff::parse_section(nlohmann::json::parse(sample.dump()));
  EXPECT_EQ(reparsed.state, ff::section_t::state_e::valid);
}

TEST(StreamFrameFeedbackParser, MalformedIntegersVersionsAndNoncesAreRefusedByField) {
  struct case_t {
    const char *field;
    nlohmann::json value;
    ff::section_t::state_e state;
    const char *reason;
  };
  using st = ff::section_t::state_e;
  const auto over64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
  const std::vector<case_t> cases {
    {"version", 2, st::unsupported_version, "unsupported_version"},
    {"version", 0, st::unsupported_version, "unsupported_version"},
    {"version", 1.0, st::malformed, "malformed_version"},
    {"version", "1", st::malformed, "malformed_version"},
    {"version", true, st::malformed, "malformed_version"},
    {"connection_nonce", "0123456789ABCDEF0123456789abcdef", st::malformed, "malformed_connection_nonce"},
    {"connection_nonce", "0123456789abcdef0123456789abcde", st::malformed, "malformed_connection_nonce"},
    {"connection_nonce", "0123456789abcdef0123456789abcdef0", st::malformed, "malformed_connection_nonce"},
    {"connection_nonce", "00000000000000000000000000000000", st::malformed, "malformed_connection_nonce"},
    {"connection_nonce", "0123456789abcdeg0123456789abcdef", st::malformed, "malformed_connection_nonce"},
    {"connection_nonce", 12345, st::malformed, "malformed_connection_nonce"},
    {"launch_nonce", "5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A", st::malformed, "malformed_launch_nonce"},
    {"launch_nonce", "00000000000000000000000000000000", st::malformed, "malformed_launch_nonce"},
    {"launch_nonce", "5a5a", st::malformed, "malformed_launch_nonce"},
    {"launch_nonce", nullptr, st::malformed, "malformed_launch_nonce"},
    // Absolute coherence, even for a first snapshot: units <= cursor and bytes >= units.
    {"decode_units_submitted_total", 2, st::malformed, "malformed_totals_incoherent"},
    {"payload_bytes_total", 0, st::malformed, "malformed_payload_bytes_total"},
    {"native_epoch", 0, st::malformed, "malformed_native_epoch"},
    {"native_epoch", -1, st::malformed, "malformed_native_epoch"},
    {"native_epoch", over64, st::malformed, "malformed_native_epoch"},
    {"native_epoch", 1.5, st::malformed, "malformed_native_epoch"},
    {"snapshot_sequence", "7", st::malformed, "malformed_snapshot_sequence"},
    {"snapshot_sequence", nullptr, st::malformed, "malformed_snapshot_sequence"},
    {"last_frame_index", 4294967296ull, st::malformed, "malformed_last_frame_index"},
    {"last_frame_index", true, st::malformed, "malformed_last_frame_index"},
    {"decode_units_submitted_total", 0, st::malformed, "malformed_decode_units_submitted_total"},
    {"payload_bytes_total", 1e3, st::malformed, "malformed_payload_bytes_total"},
    {"monotonic_timestamp_ms", -5, st::malformed, "malformed_monotonic_timestamp_ms"},
    {"last_submission_timestamp_ms", 10'001, st::malformed, "malformed_last_submission_timestamp_ms"},
    {"unexpected", 1, st::malformed, "malformed_unexpected_field"},
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(std::string {c.field} + "=" + c.value.dump());
    auto sample = sample_with({});
    sample["frame_submission"][c.field] = c.value;
    const auto section = ff::parse_section(sample);
    EXPECT_EQ(section.state, c.state);
    EXPECT_EQ(std::string {section.reason}, c.reason);
  }
  for (const char *missing : {"version", "launch_nonce", "connection_nonce", "native_epoch", "snapshot_sequence", "last_frame_index",
                              "decode_units_submitted_total", "payload_bytes_total", "monotonic_timestamp_ms",
                              "last_submission_timestamp_ms"}) {
    auto sample = sample_with({});
    sample["frame_submission"].erase(missing);
    EXPECT_NE(ff::parse_section(sample).state, ff::section_t::state_e::valid) << missing;
  }
  for (const auto &section : {nlohmann::json(nullptr), nlohmann::json(1), nlohmann::json::array(), nlohmann::json("x")}) {
    nlohmann::json sample {{"frame_submission", section}};
    const auto parsed = ff::parse_section(sample);
    EXPECT_EQ(parsed.state, ff::section_t::state_e::malformed);
    EXPECT_EQ(std::string {parsed.reason}, "malformed_section");
  }
  {
    auto sample = sample_with({.cursor = 5, .units = 5, .bytes = 4});
    const auto parsed = ff::parse_section(sample);
    EXPECT_EQ(std::string {parsed.reason}, "malformed_totals_incoherent");
  }
  // An unknown version is unsupported whatever else it carries; its other fields are not judged.
  nlohmann::json future {{"frame_submission", {{"version", 2}, {"new_field", "x"}}}};
  EXPECT_EQ(ff::parse_section(future).state, ff::section_t::state_e::unsupported_version);
}

TEST(StreamFrameFeedbackParser, ReceiptsSerializeOnlyTheVersionedContractFields) {
  encoder_t encoder {50};
  ASSERT_TRUE(encoder.open(18'000));
  scope_fixture_t f;
  for (int frame = 1; frame <= 5; ++frame) send(*f.ledger, frame, encoder.encode(frame));
  const auto baseline = ff::receipt_json(f.ingest({.epoch = 3, .sequence = 4, .cursor = 1, .units = 1, .bytes = 1, .at_ms = 100, .last_submission_ms = 100}));
  EXPECT_EQ(baseline, (nlohmann::json {{"version", 1}, {"available", false}, {"reason", "baseline"},
                                       {"native_epoch", 3}, {"snapshot_sequence", 4}}));
  const auto receipt = f.ingest({.epoch = 3, .sequence = 5, .cursor = 5, .units = 4, .bytes = 30, .at_ms = 180, .last_submission_ms = 170});
  const auto joined = ff::receipt_json(receipt);
  EXPECT_EQ(joined, (nlohmann::json {
    {"version", 1}, {"available", true}, {"reason", "joined"}, {"native_epoch", 3}, {"snapshot_sequence", 5},
    {"from_frame_exclusive", 1}, {"to_frame_inclusive", 5}, {"frames_emitted", 4}, {"decode_units_submitted", 3},
    {"frames_absent_at_submission", 1}, {"payload_bytes", 29}, {"client_interval_ms", 80},
    {"encoder_setting", {{"encoder_lifetime", 50}, {"application_sequence", 1}, {"origin_revision", 10},
                         {"configured_encoder_kbps", 18'000}, {"receipt_origin", "successful_open"}}},
  }));
  EXPECT_EQ(joined.dump().find(k_nonce_a), std::string::npos);
  const auto readback = ff::readback_json(receipt, receipt.host_received_at + 250ms);
  EXPECT_EQ(readback.at("host_receipt_age_ms"), 250);
  EXPECT_FALSE(readback.contains("unavailable_frame_index"));

  auto malformed = sample_with({});
  malformed["frame_submission"]["version"] = 9;
  const auto refused = ff::receipt_json(f.scope.ingest(ff::parse_section(malformed), clock::now()));
  EXPECT_EQ(refused, (nlohmann::json {{"version", 1}, {"available", false}, {"reason", "unsupported_version"}}));
}
