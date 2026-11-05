/** @file
 * Six public bridge histories, TEXT ONLY / UNRUN in APPLY01 preparation.
 * Boundary values are caller observations, not backend doubles or proof that
 * an SDK, broker lock, codec, queue, or driver executed. Production hooks must
 * supply these observations at the named boundaries independently.
 */
#include "src/video_bitrate_application.h"

#include <gtest/gtest.h>
#include <new>
#include <type_traits>

namespace {
  namespace app = video::bitrate_application;
  using app::adapter_t;
  constexpr auto native = app::backend_e::native_nvenc;
  constexpr auto avcodec = app::backend_e::avcodec;
  constexpr auto nv_submit = app::submit_stage_e::nvenc_encode_picture_success;
  constexpr auto av_submit = app::submit_stage_e::avcodec_send_frame_success;
  constexpr auto nv_output = app::output_stage_e::nvenc_output_timestamp;
  constexpr auto av_output = app::output_stage_e::avcodec_packet_pts;
  constexpr app::avcodec_rate_t exact_a {10'000'000, 10'000'000, 10'000'000, false};

  static_assert(!std::is_copy_constructible_v<adapter_t>);
  static_assert(!std::is_move_constructible_v<adapter_t>);
  static_assert(!std::is_copy_constructible_v<adapter_t::update_observation_t>);
  static_assert(!std::is_move_constructible_v<adapter_t::submission_sink_t>);
  static_assert(std::is_nothrow_destructible_v<adapter_t::submission_sink_t>);
}

TEST(VideoBitrateApplication, ActualUpdateOriginNoopFailureAndABA) {
  adapter_t instance {7, native};
  instance.nvenc_initialize_success();
  { auto input = instance.capture_before_submit(0); EXPECT_FALSE(input.accepted(nv_submit)); }
  EXPECT_FALSE(instance.take_output(nv_output, 0));

  app::origin_t request {10, 10000};
  {
    auto operation = instance.begin_native_update(request);
    request = {11, 20000};  // Later request cannot rewrite the captured R10.
    EXPECT_TRUE(operation.nvenc_reconfigure_success(10'000'000));
    EXPECT_FALSE(operation.nvenc_reconfigure_success(10'000'000));
  }
  { auto input = instance.capture_before_submit(1); ASSERT_TRUE(input.accepted(nv_submit)); }
  {
    auto operation = instance.begin_native_update(request);
    ASSERT_TRUE(operation.nvenc_reconfigure_success(20'000'000));
  }
  { auto input = instance.capture_before_submit(2); ASSERT_TRUE(input.accepted(nv_submit)); }
  {
    auto operation = instance.begin_native_update(app::origin_t {12, 10000});
    ASSERT_TRUE(operation.nvenc_reconfigure_success(10'000'000));
  }
  { auto input = instance.capture_before_submit(3); ASSERT_TRUE(input.accepted(nv_submit)); }
  // Helper bypass/supersession has no observation entrypoint at all. Its actual
  // callback exclusion is an existing-helper/source obligation, not an SDK fake.
  {
    auto backend_noop = instance.begin_native_update(app::origin_t {13, 10000});
    backend_noop.unchanged();
    EXPECT_FALSE(backend_noop.nvenc_reconfigure_success(10'000'000));
  }
  { auto input = instance.capture_before_submit(4); ASSERT_TRUE(input.accepted(nv_submit)); }
  const auto reaffirmed = instance.take_output(nv_output, 4);
  ASSERT_TRUE(reaffirmed);
  EXPECT_EQ(reaffirmed->authority_revision(), 12u);
  EXPECT_EQ(reaffirmed->application_sequence(), 4u);  // Open, A, B, A.
  { auto failure = instance.begin_native_update(app::origin_t {14, 6000}); }
  { auto noop = instance.begin_native_update(app::origin_t {15, 10000}); noop.unchanged(); }
  { auto input = instance.capture_before_submit(5); EXPECT_FALSE(input.accepted(nv_submit)); }
  EXPECT_FALSE(instance.take_output(nv_output, 5));

  const auto b = instance.take_output(nv_output, 2);
  const auto a = instance.take_output(nv_output, 1);
  const auto aba = instance.take_output(nv_output, 3);
  ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(aba);
  EXPECT_EQ(a->authority_revision(), 10u);
  EXPECT_EQ(b->authority_revision(), 11u);
  EXPECT_EQ(aba->authority_revision(), 12u);
  EXPECT_EQ(a->configured_encoder_kbps(), aba->configured_encoder_kbps());
  EXPECT_LT(a->application_sequence(), aba->application_sequence());
}

TEST(VideoBitrateApplication, SlowOpenKeepsOriginRequiresUsableOuterAndExactRate) {
  // A fresh candidate-only instance must refuse a successfully submitted input
  // before usable publication. No retirement/uncertainty event masks premature
  // publication; the positive slow-open histories below use separate instances.
  adapter_t candidate_only {7, avcodec};
  candidate_only.avcodec_open_success(app::origin_t {20, 10000}, exact_a, exact_a);
  { auto input = candidate_only.capture_before_submit(1); EXPECT_FALSE(input.accepted(av_submit)); }
  EXPECT_FALSE(candidate_only.take_output(av_output, 1));

  for (const auto newer_kbps : {10000, 20000}) {
    adapter_t instance {7, avcodec};
    std::optional<app::origin_t> request = app::origin_t {20, 10000};
    const auto actual_open_origin = request;
    request = app::origin_t {21, newer_kbps};
    instance.avcodec_open_success(actual_open_origin, exact_a, exact_a);
    EXPECT_FALSE(instance.take_output(av_output, 1));  // Acceptance alone publishes nothing.
    request = app::origin_t {22, newer_kbps};
    ASSERT_TRUE(instance.publish_usable_avcodec_open());
    EXPECT_FALSE(instance.publish_usable_avcodec_open());
    { auto input = instance.capture_before_submit(1); ASSERT_TRUE(input.accepted(av_submit)); }
    const auto output = instance.take_output(av_output, 1);
    ASSERT_TRUE(output);
    EXPECT_EQ(output->authority_revision(), 20u);
    EXPECT_EQ(output->configured_encoder_kbps(), 10000);
    EXPECT_NE(output->authority_revision(), request->revision);
  }
  adapter_t outer_failure {7, avcodec};
  outer_failure.avcodec_open_success(app::origin_t {20, 10000}, exact_a, exact_a);
  outer_failure.retire();
  EXPECT_FALSE(outer_failure.publish_usable_avcodec_open());
  { auto input = outer_failure.capture_before_submit(1); EXPECT_FALSE(input.accepted(av_submit)); }

  // Failed attempts do not call open_success. The actual successful retry has
  // its own literal representations; no nominal or maximum-field substitution.
  for (const auto after : {app::avcodec_rate_t {9'999'999, 0, 10'000'000, true},
                           app::avcodec_rate_t {9'000'000, 9'000'000, 10'000'000, false}}) {
    adapter_t mismatch {7, avcodec};
    mismatch.avcodec_open_success(app::origin_t {20, 10000}, exact_a, after);
    EXPECT_FALSE(mismatch.publish_usable_avcodec_open());
    { auto input = mismatch.capture_before_submit(1); EXPECT_FALSE(input.accepted(av_submit)); }
    EXPECT_FALSE(mismatch.take_output(av_output, 1));
  }
  adapter_t minus_one {7, avcodec};
  const app::avcodec_rate_t vbr {9'999'999, 0, 10'000'000, true};
  minus_one.avcodec_open_success(app::origin_t {20, 10000}, vbr, vbr);
  EXPECT_FALSE(minus_one.publish_usable_avcodec_open());
}

TEST(VideoBitrateApplication, EarlyNativeOpenCannotBorrowLaterOriginAndFallbackOwnsOrigin) {
  for (const auto later_kbps : {10000, 20000}) {
    adapter_t early_native {7, native};
    const app::origin_t earlier {30, 10000};
    early_native.nvenc_initialize_success();  // Deliberately unattributed native open.
    const app::origin_t later {31, later_kbps};
    early_native.nvenc_initialize_success();  // Repeated init is no new operation.
    EXPECT_FALSE(early_native.publish_usable_avcodec_open());
    { auto input = early_native.capture_before_submit(1); EXPECT_FALSE(input.accepted(nv_submit)); }
    EXPECT_FALSE(early_native.take_output(nv_output, 1));
    EXPECT_NE(earlier.revision, later.revision);
    // A later real different-rate native update can establish its own history.
    {
      auto update = early_native.begin_native_update(app::origin_t {32, 6000});
      ASSERT_TRUE(update.nvenc_reconfigure_success(6'000'000));
    }
    { auto input = early_native.capture_before_submit(2); ASSERT_TRUE(input.accepted(nv_submit)); }
    const auto output = early_native.take_output(nv_output, 2);
    ASSERT_TRUE(output);
    EXPECT_EQ(output->application_sequence(), 2u);
    EXPECT_EQ(output->authority_revision(), 32u);

    adapter_t fallback {7, avcodec};
    const app::avcodec_rate_t rate {static_cast<std::int64_t>(later_kbps) * 1000,
      static_cast<std::int64_t>(later_kbps) * 1000, static_cast<std::int64_t>(later_kbps) * 1000, false};
    fallback.avcodec_open_success(later, rate, rate);
    ASSERT_TRUE(fallback.publish_usable_avcodec_open());
    { auto input = fallback.capture_before_submit(1); ASSERT_TRUE(input.accepted(av_submit)); }
    const auto fallback_output = fallback.take_output(av_output, 1);
    ASSERT_TRUE(fallback_output);
    EXPECT_EQ(fallback_output->authority_revision(), 31u);
    EXPECT_NE(fallback_output->scope().encoder_lifetime, output->scope().encoder_lifetime);
  }
  for (const char *case_name : {"initial", "synchronized", "probe", "config_only"}) {
    SCOPED_TRACE(case_name);
    adapter_t absent {7, avcodec};
    absent.avcodec_open_success(std::nullopt, exact_a, exact_a);
    EXPECT_FALSE(absent.publish_usable_avcodec_open());
    { auto input = absent.capture_before_submit(1); EXPECT_FALSE(input.accepted(av_submit)); }
    EXPECT_FALSE(absent.take_output(av_output, 1));
  }
}

TEST(VideoBitrateApplication, AttributedNativeOpenPublishesOnlyAtUsableOuterAndKeepsItsOrigin) {
  adapter_t premature {7, native};
  premature.nvenc_initialize_success(app::origin_t {20, 10000}, 10'000'000, true);
  { auto input = premature.capture_before_submit(0); EXPECT_FALSE(input.accepted(nv_submit)); }
  EXPECT_FALSE(premature.publish_usable_nvenc_open());
  EXPECT_FALSE(premature.take_output(nv_output, 0));

  adapter_t instance {7, native};
  app::origin_t request {20, 10000};
  instance.nvenc_initialize_success(request, 10'000'000, true);
  EXPECT_FALSE(instance.publish_usable_avcodec_open());
  request = {21, 20000};  // A later poll is not this initialization's origin.
  instance.nvenc_initialize_success(request, 20'000'000, true);
  ASSERT_TRUE(instance.publish_usable_nvenc_open());
  EXPECT_FALSE(instance.publish_usable_nvenc_open());
  { auto input = instance.capture_before_submit(0); ASSERT_TRUE(input.accepted(nv_submit)); }
  { auto update = instance.begin_native_update(app::origin_t {22, 6000});
    ASSERT_TRUE(update.nvenc_reconfigure_success(6'000'000)); }
  { auto input = instance.capture_before_submit(1); ASSERT_TRUE(input.accepted(nv_submit)); }
  const auto old = instance.take_output(nv_output, 0);
  const auto newer = instance.take_output(nv_output, 1);
  ASSERT_TRUE(old); ASSERT_TRUE(newer);
  EXPECT_EQ(old->origin(), video::bitrate_provenance::receipt_origin_e::successful_open);
  EXPECT_EQ(old->authority_revision(), 20u);
  EXPECT_EQ(old->configured_encoder_kbps(), 10000);
  EXPECT_EQ(old->application_sequence(), 1u);
  EXPECT_EQ(newer->authority_revision(), 22u);
  EXPECT_EQ(newer->configured_encoder_kbps(), 6000);
  EXPECT_EQ(newer->application_sequence(), 2u);
}

TEST(VideoBitrateApplication, AttributedNativeOpenRefusesMissingOriginRatesAndRetiredCandidates) {
  struct invalid_t { std::optional<app::origin_t> origin; std::uint64_t bps; bool cbr; };
  for (const auto &value : {invalid_t {std::nullopt, 10'000'000, true},
                            invalid_t {app::origin_t {0, 10000}, 10'000'000, true},
                            invalid_t {app::origin_t {20, 0}, 10'000'000, true},
                            invalid_t {app::origin_t {20, 10000}, 0, true},
                            invalid_t {app::origin_t {20, 10000}, 9'999'999, true},
                            invalid_t {app::origin_t {20, 10000}, 10'000'000, false}}) {
    adapter_t instance {7, native};
    instance.nvenc_initialize_success(value.origin, value.bps, value.cbr);
    EXPECT_FALSE(instance.publish_usable_nvenc_open());
    { auto input = instance.capture_before_submit(0); EXPECT_FALSE(input.accepted(nv_submit)); }
    EXPECT_FALSE(instance.take_output(nv_output, 0));
  }
  for (const bool retire : {false, true}) {
    adapter_t instance {7, native};
    instance.nvenc_initialize_success(app::origin_t {20, 10000}, 10'000'000, true);
    if (retire) instance.retire(); else instance.setting_unavailable();
    EXPECT_FALSE(instance.publish_usable_nvenc_open());
  }
  adapter_t updated {7, native};
  updated.nvenc_initialize_success(app::origin_t {20, 10000}, 10'000'000, true);
  { auto update = updated.begin_native_update(app::origin_t {21, 6000});
    ASSERT_TRUE(update.nvenc_reconfigure_success(6'000'000)); }
  EXPECT_FALSE(updated.publish_usable_nvenc_open());
  adapter_t wrong_backend {7, avcodec};
  wrong_backend.nvenc_initialize_success(app::origin_t {20, 10000}, 10'000'000, true);
  EXPECT_FALSE(wrong_backend.publish_usable_nvenc_open());
}

TEST(VideoBitrateApplication, AnyNativeUpdateAttemptBeforeUsablePublicationSupersedesTheStagedOpen) {
  // An SDK error/early return leaves the attempt unfinished; a same-rate update
  // is an explicit no-op. Neither may leave the earlier staged open publishable.
  for (const bool noop : {false, true}) {
    SCOPED_TRACE(noop ? "same-rate no-op" : "unfinished attempt");
    adapter_t instance {7, native};
    instance.nvenc_initialize_success(app::origin_t {20, 10000}, 10'000'000, true);
    {
      auto update = instance.begin_native_update(app::origin_t {21, 10000});
      if (noop) update.unchanged();
    }
    EXPECT_FALSE(instance.publish_usable_nvenc_open());
    { auto input = instance.capture_before_submit(0); EXPECT_FALSE(input.accepted(nv_submit)); }
    EXPECT_FALSE(instance.take_output(nv_output, 0));
  }

  // After usable publication, a same-rate no-op still keeps the open's record.
  adapter_t published {7, native};
  published.nvenc_initialize_success(app::origin_t {20, 10000}, 10'000'000, true);
  ASSERT_TRUE(published.publish_usable_nvenc_open());
  { auto update = published.begin_native_update(app::origin_t {21, 10000}); update.unchanged(); }
  { auto input = published.capture_before_submit(0); ASSERT_TRUE(input.accepted(nv_submit)); }
  const auto kept = published.take_output(nv_output, 0);
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->origin(), video::bitrate_provenance::receipt_origin_e::successful_open);
  EXPECT_EQ(kept->authority_revision(), 20u);
  EXPECT_EQ(kept->application_sequence(), 1u);
}

TEST(VideoBitrateApplication, InnerAcceptedSubmitSurvivesOuterErrorsAndDelayedExactOutput) {
  adapter_t ffmpeg {7, avcodec};
  ffmpeg.avcodec_open_success(app::origin_t {40, 10000}, exact_a, exact_a);
  ASSERT_TRUE(ffmpeg.publish_usable_avcodec_open());
  { auto input = ffmpeg.capture_before_submit(100); ASSERT_TRUE(input.accepted(av_submit)); }
  // EAGAIN/no packet has no output entrypoint. A later call's PTS cannot replace
  // the actual delayed packet PTS, even if receiving later returns an error.
  { auto input = ffmpeg.capture_before_submit(101); ASSERT_TRUE(input.accepted(av_submit)); }
  EXPECT_FALSE(ffmpeg.take_output(av_output, 999));
  const auto delayed = ffmpeg.take_output(av_output, 100);
  ASSERT_TRUE(delayed);
  EXPECT_EQ(delayed->input_pts(), 100);
  const auto next = ffmpeg.take_output(av_output, 101);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->input_pts(), 101);
  { auto failed_send = ffmpeg.capture_before_submit(102); }
  EXPECT_FALSE(ffmpeg.take_output(av_output, 102));

  adapter_t nvenc {7, native};
  { auto update = nvenc.begin_native_update(app::origin_t {40, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
  {
    auto input = nvenc.capture_before_submit(200);
    ASSERT_TRUE(input.accepted(nv_submit));  // Inner acceptance precedes wait/lock failure.
    EXPECT_FALSE(input.accepted(nv_submit));
  }
  { auto pre_submit_failure = nvenc.capture_before_submit(201); }
  EXPECT_FALSE(nvenc.take_output(nv_output, 201));
  EXPECT_FALSE(nvenc.take_output(av_output, 200));  // Wrong named stage cannot consume it.
  const auto old = nvenc.take_output(nv_output, 200);
  ASSERT_TRUE(old);
  EXPECT_EQ(old->input_pts(), 200);
  EXPECT_FALSE(nvenc.take_output(nv_output, 200));
}

TEST(VideoBitrateApplication, ConcurrentInstancesRecreationAndLateRetirementStayDisjoint) {
  adapter_t first {7, native};
  adapter_t other {8, native};
  for (auto *instance : {&first, &other}) {
    auto update = instance->begin_native_update(app::origin_t {50, 10000});
    ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000));
    auto input = instance->capture_before_submit(1);
    ASSERT_TRUE(input.accepted(nv_submit));
  }
  const auto packet_value = first.take_output(nv_output, 1);
  const auto other_value = other.take_output(nv_output, 1);
  ASSERT_TRUE(packet_value); ASSERT_TRUE(other_value);
  EXPECT_NE(packet_value->scope().encoder_lifetime, other_value->scope().encoder_lifetime);
  EXPECT_NE(packet_value->scope().stream_generation, other_value->scope().stream_generation);
  first.retire();
  EXPECT_EQ(packet_value->authority_revision(), 50u);

  alignas(adapter_t) std::byte storage[sizeof(adapter_t)];
  auto *old = new (storage) adapter_t {7, native};
  { auto update = old->begin_native_update(app::origin_t {50, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
  auto stale_input = old->capture_before_submit(1);
  auto stale_update = old->begin_native_update(app::origin_t {51, 6000});
  old->retire();
  old->~adapter_t();
  auto *replacement = new (storage) adapter_t {7, native};
  { auto update = replacement->begin_native_update(app::origin_t {50, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
  auto fresh = replacement->capture_before_submit(1);
  EXPECT_FALSE(stale_input.accepted(nv_submit));
  EXPECT_FALSE(stale_update.nvenc_reconfigure_success(6'000'000));
  ASSERT_TRUE(fresh.accepted(nv_submit));
  const auto replaced = replacement->take_output(nv_output, 1);
  ASSERT_TRUE(replaced);
  EXPECT_EQ(replaced->scope().stream_generation, packet_value->scope().stream_generation);
  EXPECT_NE(replaced->scope().encoder_lifetime, packet_value->scope().encoder_lifetime);
  replacement->~adapter_t();
  EXPECT_EQ(replaced->configured_encoder_kbps(), 10000);
}

TEST(VideoBitrateApplication, InvalidFullDuplicateAndExhaustedMetadataRefusesOnlyObservation) {
  adapter_t full {7, native};
  { auto update = full.begin_native_update(app::origin_t {60, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
  for (std::int64_t pts = 0; pts < 64; ++pts) {
    auto input = full.capture_before_submit(pts);
    ASSERT_TRUE(input.accepted(nv_submit));
  }
  { auto overflow = full.capture_before_submit(64); EXPECT_FALSE(overflow.accepted(nv_submit)); }
  EXPECT_FALSE(full.take_output(nv_output, 64));
  ASSERT_TRUE(full.take_output(nv_output, 2));
  { auto reused_slot = full.capture_before_submit(65); ASSERT_TRUE(reused_slot.accepted(nv_submit)); }
  EXPECT_FALSE(full.take_output(nv_output, std::nullopt));
  EXPECT_FALSE(full.take_output(nv_output, app::checked_timestamp(std::numeric_limits<std::uint64_t>::max())));
  ASSERT_TRUE(full.take_output(nv_output, 1));
  ASSERT_TRUE(full.take_output(nv_output, 65));
  EXPECT_FALSE(full.take_output(nv_output, 65));
  { auto duplicate = full.capture_before_submit(65); EXPECT_FALSE(duplicate.accepted(nv_submit)); }
  EXPECT_FALSE(full.take_output(nv_output, 0));  // Ambiguous input retires metadata.

  for (const auto invalid : {std::optional<std::int64_t> {}, std::optional<std::int64_t> {-1}, std::optional<std::int64_t> {0}}) {
    adapter_t instance {7, native};
    { auto update = instance.begin_native_update(app::origin_t {60, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
    { auto input = instance.capture_before_submit(1); ASSERT_TRUE(input.accepted(nv_submit)); }
    { auto input = instance.capture_before_submit(invalid); EXPECT_FALSE(input.accepted(nv_submit)); }
    EXPECT_FALSE(instance.take_output(nv_output, 1));
  }
  adapter_t uncertain {7, native};
  { auto update = uncertain.begin_native_update(app::origin_t {60, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
  auto spans_unknown = uncertain.capture_before_submit(1);
  uncertain.setting_unavailable();
  EXPECT_FALSE(spans_unknown.accepted(nv_submit));

  // Refused numeric lifetime allocation and finite operation exhaustion use
  // public resource bounds. Heap bad_alloc remains a source-path obligation.
  app::identity_source_t identities {std::numeric_limits<std::uint64_t>::max() - 1};
  adapter_t last {7, native, identities, 1};
  { auto update = last.begin_native_update(app::origin_t {60, 10000}); ASSERT_TRUE(update.nvenc_reconfigure_success(10'000'000)); }
  { auto input = last.capture_before_submit(1); ASSERT_TRUE(input.accepted(nv_submit)); }
  { auto update = last.begin_native_update(app::origin_t {61, 6000}); EXPECT_FALSE(update.nvenc_reconfigure_success(6'000'000)); }
  EXPECT_FALSE(last.take_output(nv_output, 1));
  adapter_t exhausted {7, native, identities};
  adapter_t invalid_scope {0, native};
  for (auto *instance : {&exhausted, &invalid_scope}) {
    auto update = instance->begin_native_update(app::origin_t {60, 10000});
    EXPECT_FALSE(update.nvenc_reconfigure_success(10'000'000));
    auto input = instance->capture_before_submit(1);
    EXPECT_FALSE(input.accepted(nv_submit));
    EXPECT_FALSE(instance->take_output(nv_output, 1));
  }
}

TEST(VideoBitrateApplication, CurrentWitnessRejectsInFlightAndFailedAttemptsButKeepsLiteralNoop) {
  adapter_t instance {7, native};
  instance.nvenc_initialize_success(app::origin_t {40, 20000}, 20'000'000, true);
  ASSERT_TRUE(instance.publish_usable_nvenc_open());
  const auto view = instance.current_setting_view().lock(); ASSERT_TRUE(view);
  ASSERT_TRUE(view->with_current([](const auto &) {}));
  {
    auto noop = instance.begin_native_update(app::origin_t {41, 20000});
    EXPECT_FALSE(view->with_current([](const auto &) {}));
    noop.unchanged();
  }
  EXPECT_TRUE(view->with_current([](const auto &r) { EXPECT_EQ(r.authority_revision, 40u); }));
  { auto failed = instance.begin_native_update(app::origin_t {42, 17000}); }
  EXPECT_FALSE(view->with_current([](const auto &) {}));
  {
    auto applied = instance.begin_native_update(app::origin_t {43, 17000});
    ASSERT_TRUE(applied.nvenc_reconfigure_success(17'000'000));
  }
  EXPECT_TRUE(view->with_current([](const auto &r) {
    EXPECT_EQ(r.authority_revision, 43u); EXPECT_EQ(r.configured_encoder_kbps, 17000);
  }));
  instance.retire();
  EXPECT_FALSE(view->with_current([](const auto &) {}));
}
