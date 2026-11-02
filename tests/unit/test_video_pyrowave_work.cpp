/**
 * @file tests/unit/test_video_pyrowave_work.cpp
 * @brief Public metadata API guards. No backend double or arithmetic model.
 *
 * The vectors and success values below are caller observations, not codec/GPU
 * completion evidence. Real hooks must supply them at the selected boundaries.
 * All tests are TEXT ONLY / UNRUN in the PYROWORK01 preparation wave.
 */
#include "src/video_pyrowave_work.h"

#include <gtest/gtest.h>
#include <limits>
#include <new>
#include <type_traits>

namespace {
  namespace pw = video::pyrowave_work;
  using bytes_t = std::vector<std::uint8_t>;

  static_assert(!std::is_copy_constructible_v<pw::adapter_t>);
  static_assert(!std::is_move_constructible_v<pw::adapter_t>);
  static_assert(!std::is_copy_constructible_v<pw::work_ticket_t>);
  static_assert(std::is_const_v<pw::receipt_value::value_type>);
  static_assert(std::is_nothrow_copy_constructible_v<pw::handoff_receipt_t>);

  TEST(PyroWaveWork, ConvertedABeforeTargetUpdateBRetainsAAtLaterHandoff) {
    pw::adapter_t observations;
    bytes_t buffer {1, 2, 3, 4};
    std::size_t mutable_wrapper_budget = 7001;
    auto work = observations.begin_work(pw::kind_e::imported, mutable_wrapper_budget);
    observations.complete_work(std::move(work), true, buffer);
    mutable_wrapper_budget = 9007;
    observations.prepare_cached_buffer(buffer);
    const bytes_t packet_copy {buffer};
    auto receipt = observations.take_for_packet(41, buffer, packet_copy);
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->effective_budget_argument_bytes(), 7001u);
    EXPECT_NE(receipt->effective_budget_argument_bytes(), mutable_wrapper_budget);
    EXPECT_EQ(receipt->handoff_frame_index(), 41);
    EXPECT_EQ(receipt->work_kind(), pw::kind_e::imported);
    EXPECT_FALSE(work.available());
  }

  TEST(PyroWaveWork, RetainedRepeatIsFreshBWorkAndPreservesHistoricalA) {
    pw::adapter_t observations;
    bytes_t buffer {8, 9, 10};
    auto first = observations.begin_work(pw::kind_e::packed, 7001);
    observations.complete_work(std::move(first), true, buffer);
    observations.prepare_cached_buffer(buffer);
    auto old = observations.take_for_packet(1, buffer, bytes_t {buffer});
    ASSERT_TRUE(old);
    auto repeat = observations.begin_work(pw::kind_e::retained, 9007);
    buffer = {11, 12};
    EXPECT_TRUE(observations.complete_retained(std::move(repeat), true, buffer));
    auto fresh = observations.take_for_packet(2, buffer, bytes_t {buffer});
    ASSERT_TRUE(fresh);
    EXPECT_TRUE(fresh->same_local_wrapper(*old));
    EXPECT_GT(fresh->local_work_sequence(), old->local_work_sequence());
    EXPECT_EQ(fresh->work_kind(), pw::kind_e::retained);
    EXPECT_EQ(fresh->effective_budget_argument_bytes(), 9007u);
    EXPECT_EQ(old->effective_budget_argument_bytes(), 7001u);
    EXPECT_EQ(old->completed_bitstream_bytes(), 3u);
  }

  TEST(PyroWaveWork, EntryFailureAndEmptyWorkCannotBorrowSurvivingCodecBuffer) {
    pw::adapter_t observations;
    bytes_t surviving_codec_buffer {1, 2, 3};
    auto original = observations.begin_work(pw::kind_e::blank_primer, 4096);
    observations.complete_work(std::move(original), true, surviving_codec_buffer);
    // convert() can fail before reaching any backend call. Its old converted
    // flag and bitstream may survive; only metadata is discarded here.
    observations.discard_buffer();
    observations.prepare_cached_buffer(surviving_codec_buffer);
    EXPECT_FALSE(observations.take_for_packet(1, surviving_codec_buffer, bytes_t {surviving_codec_buffer}));
    auto failed = observations.begin_work(pw::kind_e::imported, 5000);
    observations.complete_work(std::move(failed), false, surviving_codec_buffer);
    observations.prepare_cached_buffer(surviving_codec_buffer);
    EXPECT_FALSE(observations.take_for_packet(2, surviving_codec_buffer, bytes_t {surviving_codec_buffer}));
    auto empty = observations.begin_work(pw::kind_e::blank_hdr, 6000);
    bytes_t no_output;
    observations.complete_work(std::move(empty), true, no_output);
    observations.prepare_cached_buffer(surviving_codec_buffer);
    EXPECT_FALSE(observations.take_for_packet(3, surviving_codec_buffer, bytes_t {surviving_codec_buffer}));
    EXPECT_EQ(surviving_codec_buffer, (bytes_t {1, 2, 3}));
  }

  TEST(PyroWaveWork, OverwriteAndMovedFromTicketsCannotResurrectOldObservation) {
    pw::adapter_t observations;
    bytes_t buffer {5, 6};
    auto abandoned = observations.begin_work(pw::kind_e::packed, 7000);
    auto replacement = observations.begin_work(pw::kind_e::packed, 8000);
    observations.complete_work(std::move(abandoned), true, buffer);
    observations.complete_work(std::move(replacement), true, buffer);
    observations.prepare_cached_buffer(buffer);
    EXPECT_FALSE(observations.take_for_packet(1, buffer, bytes_t {buffer}));
    auto valid = observations.begin_work(pw::kind_e::packed, 9000);
    auto moved = std::move(valid);
    EXPECT_FALSE(valid.available());
    observations.complete_work(std::move(moved), true, buffer);
    observations.prepare_cached_buffer(buffer);
    auto receipt = observations.take_for_packet(2, buffer, bytes_t {buffer});
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->effective_budget_argument_bytes(), 9000u);
    observations.complete_work(std::move(valid), true, buffer);
    observations.prepare_cached_buffer(buffer);
    EXPECT_FALSE(observations.take_for_packet(3, buffer, bytes_t {buffer}));
  }

  TEST(PyroWaveWork, ExactBufferAndOnceOnlyIncreasingFrameBindingAreRequired) {
    pw::adapter_t observations;
    bytes_t buffer {1, 2, 3};
    const bytes_t different_buffer {buffer};
    auto mismatched = observations.begin_work(pw::kind_e::retained, 7000);
    observations.complete_retained(std::move(mismatched), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(8, different_buffer, bytes_t {different_buffer}));
    auto valid = observations.begin_work(pw::kind_e::retained, 7000);
    observations.complete_retained(std::move(valid), true, buffer);
    auto receipt = observations.take_for_packet(9, buffer, bytes_t {buffer});
    ASSERT_TRUE(receipt);
    EXPECT_FALSE(observations.take_for_packet(10, buffer, bytes_t {buffer}));
    auto duplicate = observations.begin_work(pw::kind_e::retained, 8000);
    observations.complete_retained(std::move(duplicate), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(10, buffer, bytes_t {buffer}));
    auto regressed = observations.begin_work(pw::kind_e::retained, 8000);
    observations.complete_retained(std::move(regressed), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(7, buffer, bytes_t {buffer}));
    auto next = observations.begin_work(pw::kind_e::retained, 8000);
    observations.complete_retained(std::move(next), true, buffer);
    EXPECT_TRUE(observations.take_for_packet(11, buffer, bytes_t {buffer}));
    EXPECT_EQ(receipt->handoff_frame_index(), 9);
  }

  TEST(PyroWaveWork, CrossInstanceSameAddressRecreationAndRetirementCannotAlias) {
    alignas(pw::adapter_t) unsigned char storage[sizeof(pw::adapter_t)];
    bytes_t buffer {1, 2};
    auto *first = ::new (static_cast<void *>(storage)) pw::adapter_t;
    auto completed = first->begin_work(pw::kind_e::retained, 7000);
    first->complete_retained(std::move(completed), true, buffer);
    auto historical = first->take_for_packet(1, buffer, bytes_t {buffer});
    ASSERT_TRUE(historical);
    auto detached = first->begin_work(pw::kind_e::packed, 7100);
    first->retire();
    EXPECT_FALSE(first->available());
    first->~adapter_t();
    auto *second = ::new (static_cast<void *>(storage)) pw::adapter_t;
    second->complete_work(std::move(detached), true, buffer);
    second->prepare_cached_buffer(buffer);
    EXPECT_FALSE(second->take_for_packet(1, buffer, bytes_t {buffer}));
    auto fresh = second->begin_work(pw::kind_e::retained, 8000);
    second->complete_retained(std::move(fresh), true, buffer);
    auto current = second->take_for_packet(2, buffer, bytes_t {buffer});
    ASSERT_TRUE(current);
    EXPECT_FALSE(current->same_local_wrapper(*historical));
    second->~adapter_t();
    EXPECT_EQ(historical->effective_budget_argument_bytes(), 7000u);
    EXPECT_EQ(current->effective_budget_argument_bytes(), 8000u);
    EXPECT_EQ(historical->authority(), pw::authority_e::unknown);
    EXPECT_EQ(current->authority(), pw::authority_e::unknown);
  }

  TEST(PyroWaveWork, EveryWorkKindKeepsUnknownAuthorityAndNamedByteStages) {
    pw::adapter_t observations;
    bytes_t buffer {3, 4, 5, 6, 7};
    std::int64_t frame = 0;
    for (auto kind : {pw::kind_e::imported, pw::kind_e::blank_primer,
                      pw::kind_e::blank_hdr, pw::kind_e::packed, pw::kind_e::retained}) {
      // Budget compliance is NOT this receipt's claim: five observed bytes
      // against an effective argument of three must remain observable.
      auto work = observations.begin_work(kind, 3);
      observations.complete_work(std::move(work), true, buffer);
      if (kind != pw::kind_e::retained) observations.prepare_cached_buffer(buffer);
      auto receipt = observations.take_for_packet(frame++, buffer, bytes_t {buffer});
      ASSERT_TRUE(receipt);
      EXPECT_EQ(receipt->authority(), pw::authority_e::unknown);
      EXPECT_EQ(receipt->provenance(), pw::provenance_e::wrapper_call_success_nonempty);
      EXPECT_EQ(receipt->work_kind(), kind);
      EXPECT_EQ(receipt->effective_budget_argument_bytes(), 3u);
      EXPECT_EQ(receipt->completed_bitstream_bytes(), 5u);
      EXPECT_EQ(receipt->generic_packet_copy_bytes(), 5u);
      // No nominal-kbps, broker-revision or owner permission accessor exists.
    }
  }

  TEST(PyroWaveWork, InvalidAndExhaustedMetadataIdentityPreservesBackendResult) {
    pw::adapter_t disabled {0};
    bytes_t buffer {1};
    auto unavailable = disabled.begin_work(pw::kind_e::retained, 4096);
    EXPECT_FALSE(unavailable.available());
    EXPECT_TRUE(disabled.complete_retained(std::move(unavailable), true, buffer));
    EXPECT_FALSE(disabled.take_for_packet(1, buffer, bytes_t {buffer}));
    pw::adapter_t finite {1};
    auto invalid_kind = finite.begin_work(static_cast<pw::kind_e>(999), 4096);
    EXPECT_FALSE(invalid_kind.available());
    auto invalid_budget = finite.begin_work(pw::kind_e::packed, 0);
    EXPECT_FALSE(invalid_budget.available());
    auto last = finite.begin_work(pw::kind_e::retained, 4096);
    EXPECT_TRUE(last.available());
    EXPECT_TRUE(finite.complete_retained(std::move(last), true, buffer));
    auto receipt = finite.take_for_packet(1, buffer, bytes_t {buffer});
    ASSERT_TRUE(receipt);
    auto exhausted = finite.begin_work(pw::kind_e::retained, 5000);
    EXPECT_FALSE(exhausted.available());
    EXPECT_TRUE(finite.complete_retained(std::move(exhausted), true, buffer));
    EXPECT_FALSE(finite.take_for_packet(2, buffer, bytes_t {buffer}));
    auto still_exhausted = finite.begin_work(pw::kind_e::retained, 6000);
    EXPECT_FALSE(still_exhausted.available());
    EXPECT_FALSE(finite.complete_retained(std::move(still_exhausted), false, buffer));
    EXPECT_EQ(receipt->local_work_sequence(), 1u);
    EXPECT_EQ(buffer, (bytes_t {1}));
  }

  TEST(PyroWaveWork, UnpreparedChangedAndWrongSizedBuffersStayUnavailable) {
    pw::adapter_t observations;
    bytes_t buffer {1, 2, 3};
    auto unprepared = observations.begin_work(pw::kind_e::packed, 4096);
    observations.complete_work(std::move(unprepared), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(1, buffer, bytes_t {buffer}));
    auto changed = observations.begin_work(pw::kind_e::packed, 4096);
    observations.complete_work(std::move(changed), true, buffer);
    buffer.push_back(4);
    observations.prepare_cached_buffer(buffer);
    EXPECT_FALSE(observations.take_for_packet(2, buffer, bytes_t {buffer}));
    auto wrong_size = observations.begin_work(pw::kind_e::retained, 4096);
    observations.complete_retained(std::move(wrong_size), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(3, buffer, bytes_t {1}));
    EXPECT_FALSE(observations.take_for_packet(4, buffer, bytes_t {buffer}));
    auto duplicate_prepare = observations.begin_work(pw::kind_e::packed, 4096);
    observations.complete_work(std::move(duplicate_prepare), true, buffer);
    observations.prepare_cached_buffer(buffer);
    observations.prepare_cached_buffer(buffer);
    EXPECT_FALSE(observations.take_for_packet(5, buffer, bytes_t {buffer}));
  }

  TEST(PyroWaveWork, InvalidAndMaximumFrameIdentityNeverWrapOrReviveMetadata) {
    pw::adapter_t observations;
    bytes_t buffer {7};
    auto invalid = observations.begin_work(pw::kind_e::retained, 4096);
    observations.complete_retained(std::move(invalid), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(-1, buffer, bytes_t {buffer}));
    auto maximum = observations.begin_work(pw::kind_e::retained, 4096);
    observations.complete_retained(std::move(maximum), true, buffer);
    auto receipt = observations.take_for_packet(std::numeric_limits<std::int64_t>::max(), buffer, bytes_t {buffer});
    ASSERT_TRUE(receipt);
    auto wrapped = observations.begin_work(pw::kind_e::retained, 5000);
    observations.complete_retained(std::move(wrapped), true, buffer);
    EXPECT_FALSE(observations.take_for_packet(0, buffer, bytes_t {buffer}));
    EXPECT_EQ(receipt->handoff_frame_index(), std::numeric_limits<std::int64_t>::max());
  }
}  // namespace
