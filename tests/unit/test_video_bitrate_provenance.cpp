// FRAME01 private proposal. TEXT ONLY: all 10 cases and trait checks UNRUN.
#include <src/video_bitrate_provenance.h>

#include <array>
#include <new>
#include <optional>
#include <type_traits>

#include <gtest/gtest.h>

namespace {
  namespace provenance = video::bitrate_provenance;
  using provenance::ledger_t;
  using provenance::receipt_origin_e;
  using provenance::scope_t;
  using provenance::setting_receipt_t;
  using provenance::submission_result_e;
  constexpr auto submitted = submission_result_e::submitted_under_captured_setting;
  constexpr auto unproven = submission_result_e::failed_or_unproven;
  constexpr scope_t scope {101, 7};

  // Data construction only: this is an explicit test assertion of a receipt,
  // not a mocked backend or a claim of actual driver evidence.
  setting_receipt_t receipt(scope_t owner, std::uint64_t application, std::uint64_t authority,
                            int kbps, receipt_origin_e origin = receipt_origin_e::backend_reconfiguration) {
    return {owner, application, authority, kbps, origin};
  }

  static_assert(!std::is_copy_constructible_v<ledger_t>);
  static_assert(!std::is_copy_assignable_v<ledger_t>);
  static_assert(!std::is_move_constructible_v<ledger_t>);
  static_assert(!std::is_move_assignable_v<ledger_t>);
  static_assert(!std::is_default_constructible_v<provenance::submission_t>);
  static_assert(!std::is_default_constructible_v<provenance::record_t>);
  static_assert(!std::is_convertible_v<provenance::record_t, setting_receipt_t>);
}

TEST(VideoBitrateProvenance, DelayedOutOfOrderOutputsKeepSettingsAcrossCut) {
  ledger_t ledger {scope};
  auto opened = receipt(scope, 1, 40, 20000, receipt_origin_e::successful_open);
  ASSERT_TRUE(ledger.observe_receipt(opened));
  const auto old_input = ledger.capture(10);
  ASSERT_TRUE(old_input);
  ASSERT_TRUE(ledger.complete(*old_input, submitted));
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 2, 41, 8000)));
  const auto new_input = ledger.capture(11);
  ASSERT_TRUE(new_input);
  ASSERT_TRUE(ledger.complete(*new_input, submitted));
  opened.configured_encoder_kbps = 999;  // Ledger must own a value copy.

  const auto newer = ledger.take(scope, 11);
  ASSERT_TRUE(newer);
  EXPECT_EQ(newer->configured_encoder_kbps(), 8000);
  EXPECT_EQ(newer->authority_revision(), 41u);
  const auto older = ledger.take(scope, 10);
  ASSERT_TRUE(older);
  EXPECT_EQ(older->configured_encoder_kbps(), 20000);
  EXPECT_EQ(older->authority_revision(), 40u);
  EXPECT_EQ(older->application_sequence(), 1u);
  EXPECT_EQ(older->submission_sequence(), old_input->sequence());
  EXPECT_EQ(older->input_pts(), 10);
  EXPECT_TRUE(older->scope() == scope);
  EXPECT_EQ(older->origin(), receipt_origin_e::successful_open);
}

TEST(VideoBitrateProvenance, SameKbpsABADistinguishesApplicationsAndRejectsReaffirmation) {
  ledger_t ledger {scope};
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 10, 20000, receipt_origin_e::successful_open)));
  const auto a = ledger.capture(1);
  ASSERT_TRUE(a);
  ASSERT_TRUE(ledger.complete(*a, submitted));
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 2, 11, 8000)));
  const auto b = ledger.capture(2);
  ASSERT_TRUE(b);
  ASSERT_TRUE(ledger.complete(*b, submitted));
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 3, 12, 20000)));
  const auto c = ledger.capture(3);
  ASSERT_TRUE(c);
  ASSERT_TRUE(ledger.complete(*c, submitted));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 4, 13, 20000)));  // No new edge.
  EXPECT_FALSE(ledger.capture(4));
  const auto last = ledger.take(scope, 3);
  const auto first = ledger.take(scope, 1);
  const auto middle = ledger.take(scope, 2);
  ASSERT_TRUE(last);
  ASSERT_TRUE(first);
  ASSERT_TRUE(middle);
  EXPECT_EQ(last->configured_encoder_kbps(), first->configured_encoder_kbps());
  EXPECT_EQ(first->authority_revision(), 10u);
  EXPECT_EQ(last->authority_revision(), 12u);
  EXPECT_EQ(last->application_sequence(), 3u);
  EXPECT_EQ(middle->configured_encoder_kbps(), 8000);
}

TEST(VideoBitrateProvenance, NoReceiptAndMissingInputCannotBorrowCurrentSetting) {
  ledger_t ledger {scope};
  EXPECT_FALSE(ledger.capture(1));  // Nominal startup config supplies no receipt.
  EXPECT_FALSE(ledger.take(scope, 1));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 2, 2, 8000)));
  const auto pending = ledger.capture(2);
  ASSERT_TRUE(pending);
  ASSERT_TRUE(ledger.complete(*pending, submitted));
  EXPECT_FALSE(ledger.capture(std::nullopt));  // Ambiguous input retires metadata only.
  EXPECT_FALSE(ledger.take(scope, 2));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 3, 3, 6000)));
  EXPECT_FALSE(ledger.capture(3));
}

TEST(VideoBitrateProvenance, FailedSubmissionAndPrematureOutputCannotReplayTickets) {
  ledger_t ledger {scope};
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  const auto failed = ledger.capture(1);
  ASSERT_TRUE(failed);
  const auto detached = *failed;
  EXPECT_FALSE(ledger.complete(*failed, unproven));
  EXPECT_FALSE(ledger.complete(detached, submitted));
  EXPECT_FALSE(ledger.take(scope, 1));

  const auto pending = ledger.capture(2);
  ASSERT_TRUE(pending);
  EXPECT_FALSE(ledger.take(scope, 2));  // Successful inner submit not yet attested.
  EXPECT_FALSE(ledger.complete(*pending, submitted));
  const auto accepted = ledger.capture(3);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(ledger.complete(*accepted, submitted));
  EXPECT_FALSE(ledger.complete(*accepted, submitted));
  EXPECT_FALSE(ledger.complete(*accepted, unproven));  // Cannot erase accepted evidence.
  ASSERT_TRUE(ledger.take(scope, 3));
  EXPECT_FALSE(ledger.take(scope, 3));
}

TEST(VideoBitrateProvenance, CapacityAndSlotReuseDoNotEvictOrAliasDelayedInputs) {
  ledger_t ledger {scope};
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  std::optional<provenance::submission_t> first;
  for (std::int64_t pts = 1; pts <= 64; ++pts) {
    const auto input = ledger.capture(pts);
    ASSERT_TRUE(input);
    if (pts == 1) first = input;
    ASSERT_TRUE(ledger.complete(*input, submitted));
  }
  EXPECT_FALSE(ledger.capture(65));
  EXPECT_FALSE(ledger.take(scope, 65));
  const auto old = ledger.take(scope, 1);
  ASSERT_TRUE(old);
  EXPECT_EQ(old->input_pts(), 1);
  const auto reused_slot = ledger.capture(66);
  ASSERT_TRUE(reused_slot);
  ASSERT_TRUE(first);
  EXPECT_NE(reused_slot->sequence(), first->sequence());
  EXPECT_FALSE(ledger.complete(*first, submitted));
  ASSERT_TRUE(ledger.complete(*reused_slot, submitted));
  const auto late = ledger.take(scope, 64);
  ASSERT_TRUE(late);
  EXPECT_EQ(late->configured_encoder_kbps(), 20000);
  ASSERT_TRUE(ledger.take(scope, 2));
  ASSERT_TRUE(ledger.take(scope, 66));
  EXPECT_FALSE(ledger.take(scope, 65));  // Overflow was not recreated on lookup.
}

TEST(VideoBitrateProvenance, DuplicateOrRegressedInputRetiresStickyPtsHistory) {
  for (const std::int64_t ambiguous : {10, 9}) {
    ledger_t ledger {scope};
    ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
    const auto input = ledger.capture(10);
    ASSERT_TRUE(input);
    ASSERT_TRUE(ledger.complete(*input, submitted));
    EXPECT_FALSE(ledger.capture(ambiguous));
    EXPECT_FALSE(ledger.take(scope, 10));  // May now alias an untracked input.
    EXPECT_FALSE(ledger.capture(11));
    EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 2, 2, 8000)));
  }
  ledger_t full {scope};
  ASSERT_TRUE(full.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  for (std::int64_t pts = 1; pts <= 64; ++pts) {
    const auto input = full.capture(pts);
    ASSERT_TRUE(input);
    ASSERT_TRUE(full.complete(*input, submitted));
  }
  EXPECT_FALSE(full.capture(65));
  ASSERT_TRUE(full.take(scope, 1));
  EXPECT_FALSE(full.capture(65));  // Overflow PTS stays reserved after space returns.
  EXPECT_FALSE(full.take(scope, 2));
}

TEST(VideoBitrateProvenance, LifetimeAndGenerationFenceTicketsAndDetachedValues) {
  std::optional<provenance::record_t> queued;
  std::optional<provenance::submission_t> old_ticket;
  {
    ledger_t old {scope};
    ASSERT_TRUE(old.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
    old_ticket = old.capture(1);
    ASSERT_TRUE(old_ticket);
    ASSERT_TRUE(old.complete(*old_ticket, submitted));
    queued = old.take(scope, 1);
    ASSERT_TRUE(queued);
    old.retire();
    EXPECT_FALSE(old.capture(2));
    EXPECT_FALSE(old.observe_receipt(receipt(scope, 2, 2, 8000)));
  }
  EXPECT_EQ(queued->configured_encoder_kbps(), 20000);  // Value survives destruction.
  for (const scope_t next : std::array<scope_t, 2> {{{102, 7}, {101, 8}}}) {
    ledger_t fresh {next};
    ASSERT_TRUE(fresh.observe_receipt(receipt(next, 1, 1, 8000, receipt_origin_e::successful_open)));
    const auto input = fresh.capture(1);
    ASSERT_TRUE(input);
    EXPECT_FALSE(fresh.complete(*old_ticket, submitted));
    EXPECT_FALSE(fresh.take(scope, 1));
    ASSERT_TRUE(fresh.complete(*input, submitted));
    const auto output = fresh.take(next, 1);
    ASSERT_TRUE(output);
    EXPECT_EQ(output->configured_encoder_kbps(), 8000);
    EXPECT_TRUE(output->scope() == next);
  }
  ledger_t invalid {{0, 7}};
  EXPECT_FALSE(invalid.observe_receipt(receipt({0, 7}, 1, 1, 20000, receipt_origin_e::successful_open)));
  EXPECT_FALSE(invalid.capture(1));

  // Deliberately breach caller scope uniqueness to guard local handle custody:
  // the library still cannot prove that either external receipt was truthful.
  ledger_t left {scope};
  ledger_t right {scope};
  ASSERT_TRUE(left.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  ASSERT_TRUE(right.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  const auto left_input = left.capture(1);
  const auto right_input = right.capture(1);
  ASSERT_TRUE(left_input);
  ASSERT_TRUE(right_input);
  EXPECT_FALSE(right.complete(*left_input, submitted));
  EXPECT_FALSE(left.complete(*right_input, submitted));
  ASSERT_TRUE(left.complete(*left_input, submitted));
  ASSERT_TRUE(right.complete(*right_input, submitted));

  alignas(ledger_t) std::byte storage[sizeof(ledger_t)];
  auto *at_address = ::new (static_cast<void *>(storage)) ledger_t {scope};
  ASSERT_TRUE(at_address->observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  const auto before_reuse = at_address->capture(1);
  ASSERT_TRUE(before_reuse);
  at_address->~ledger_t();
  at_address = ::new (static_cast<void *>(storage)) ledger_t {scope};
  ASSERT_TRUE(at_address->observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  const auto after_reuse = at_address->capture(1);
  ASSERT_TRUE(after_reuse);
  EXPECT_FALSE(at_address->complete(*before_reuse, submitted));
  EXPECT_TRUE(at_address->complete(*after_reuse, submitted));
  at_address->~ledger_t();
}

TEST(VideoBitrateProvenance, SettingChangesDuringWorkInvalidateOnlyPendingCaptures) {
  ledger_t ledger {scope};
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  const auto accepted = ledger.capture(1);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(ledger.complete(*accepted, submitted));
  const auto spans_change = ledger.capture(2);
  ASSERT_TRUE(spans_change);
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 2, 2, 8000)));
  EXPECT_FALSE(ledger.complete(*spans_change, submitted));
  EXPECT_FALSE(ledger.take(scope, 2));
  const auto spans_unknown = ledger.capture(3);
  ASSERT_TRUE(spans_unknown);
  ledger.setting_unavailable();
  EXPECT_FALSE(ledger.complete(*spans_unknown, submitted));
  EXPECT_FALSE(ledger.capture(4));
  const auto old_output = ledger.take(scope, 1);
  ASSERT_TRUE(old_output);
  EXPECT_EQ(old_output->configured_encoder_kbps(), 20000);
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 2, 2, 8000)));  // Receipt replay.
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 3, 3, 6000)));
  const auto new_input = ledger.capture(5);
  ASSERT_TRUE(new_input);
  ASSERT_TRUE(ledger.complete(*new_input, submitted));
  ASSERT_TRUE(ledger.take(scope, 5));
}

TEST(VideoBitrateProvenance, ReceiptValidationCannotPromoteNominalOrRegressedAuthority) {
  ledger_t ledger {scope};
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 1, 0, 20000, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.observe_receipt(receipt({999, 7}, 1, 1, 20000, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 0, 1, 20000, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 1, 1, 0, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 1, 1, -1, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::unavailable)));
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 10, 20000, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 2, 11, 8000, receipt_origin_e::successful_open)));
  EXPECT_FALSE(ledger.capture(1));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 1, 11, 8000)));
  EXPECT_FALSE(ledger.observe_receipt(receipt(scope, 2, 9, 8000)));
  EXPECT_FALSE(ledger.capture(2));
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 2, 11, 8000)));
  const auto known = ledger.capture(3);
  ASSERT_TRUE(known);
  ASSERT_TRUE(ledger.complete(*known, submitted));
  const auto output = ledger.take(scope, 3);
  ASSERT_TRUE(output);
  EXPECT_EQ(output->authority_revision(), 11u);
  EXPECT_EQ(output->application_sequence(), 2u);
}

TEST(VideoBitrateProvenance, MissingUnmatchedAndRepeatedOutputNeverBorrowOrConsumeOtherPts) {
  ledger_t ledger {scope};
  ASSERT_TRUE(ledger.observe_receipt(receipt(scope, 1, 1, 20000, receipt_origin_e::successful_open)));
  const auto a = ledger.capture(100);
  const auto b = ledger.capture(101);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(ledger.complete(*a, submitted));
  ASSERT_TRUE(ledger.complete(*b, submitted));
  EXPECT_FALSE(ledger.take(scope, std::nullopt));
  EXPECT_FALSE(ledger.take(scope, -1));
  EXPECT_FALSE(ledger.take(scope, 999));
  EXPECT_FALSE(ledger.take({101, 8}, 100));
  const auto second = ledger.take(scope, 101);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->input_pts(), 101);
  EXPECT_FALSE(ledger.take(scope, 101));  // No implicit multi-packet grouping.
  const auto first = ledger.take(scope, 100);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->input_pts(), 100);
  EXPECT_FALSE(ledger.take(scope, 100));
}
