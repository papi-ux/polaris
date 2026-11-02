/** @file Tests of the production packet-budget join; no encoder, socket or timer is opened. */
#include <gtest/gtest.h>
#include <future>

#include "src/config.h"
#include "src/live_tuning_fec.h"
#include "src/stream_applied_pacing.h"
#include "src/stream_fec.h"
#include "src/video_bitrate_application.h"

namespace {
  using namespace std::literals;
  namespace applied = stream_applied_pacing;
  namespace provenance = video::bitrate_provenance;
  namespace application = video::bitrate_application;
  using cap_t = adaptive_bitrate::live_pacing_cap_t;
  constexpr std::uint64_t generation = 605;
  constexpr std::size_t blocksize = 1416;

  stream_bitrate::request_t request() {
    stream_bitrate::request_t result;
    stream_bitrate::split_request(result, 20000, 10, 512);
    return result;
  }

  std::optional<provenance::record_t> frame(int kbps = 8000, std::uint64_t revision = 10,
                                          std::int64_t pts = 7, std::uint64_t owner = generation) {
    provenance::ledger_t ledger {{9, owner}};
    if (!ledger.observe_receipt({{9, owner}, 1, revision, kbps, provenance::receipt_origin_e::successful_open})) return {};
    const auto ticket = ledger.capture(pts);
    if (!ticket || !ledger.complete(*ticket, provenance::submission_result_e::submitted_under_captured_setting)) return {};
    return ledger.take({9, owner}, pts);
  }

  applied::selection_t select(const provenance::record_t *record,
                              std::optional<cap_t> cap = cap_t {generation, 11, 16000},
                              bool enabled = true, std::size_t prefix = 0) {
    return applied::select_rate(enabled, request(), 100, blocksize, prefix, generation, 7, record, cap);
  }

  void expect_same_rate(const stream_pacing::rate_t &a, const stream_pacing::rate_t &b) {
    EXPECT_EQ(a.enabled, b.enabled);
    EXPECT_EQ(a.session_rate, b.session_rate);
    EXPECT_EQ(a.packets_in_1ms, b.packets_in_1ms);
    EXPECT_EQ(a.packets_per_second, b.packets_per_second);
    EXPECT_EQ(a.bits_per_second, b.bits_per_second);
    EXPECT_EQ(a.bits_per_packet, b.bits_per_packet);
  }
}

TEST(StreamAppliedPacing, HandshakeAdmissionReservesAudioAndControlWithoutApplyingWarpTwice) {
  const auto value = frame(16988);
  ASSERT_TRUE(value);
  const auto selected = select(&*value, cap_t {generation, 11, 20000});
  ASSERT_TRUE(selected.applied());
  // 20,000 total -> 18,000 after F0 -> 16,988 video. Only the 1,012
  // audio/control allocation is reserved from the packet admission ceiling.
  EXPECT_EQ(request().encoder_kbps, 16988);
  EXPECT_EQ(selected.rate.bits_per_second, 18'988'000u);
  auto warped = request();
  warped.warp_factor = 3;  // split_kbps is already the handshake's final warped total.
  const auto same = applied::select_rate(true, warped, 100, blocksize, 0, generation, 7, &*value,
    adaptive_bitrate::live_pacing_cap_t {generation, 11, 20000});
  expect_same_rate(selected.rate, same.rate);
}

TEST(StreamAppliedPacing, QueuedOutputKeepsItsOwnAppliedRevisionDespiteANewerUp) {
  const auto old = frame();
  const auto newer = frame(16000, 11);
  ASSERT_TRUE(old);
  ASSERT_TRUE(newer);
  const auto before = select(&*old, cap_t {generation, 10, 8000});
  const auto after_up = select(&*old);
  ASSERT_TRUE(before.applied());
  ASSERT_TRUE(after_up.applied());
  expect_same_rate(before.rate, after_up.rate);
  EXPECT_GT(select(&*newer).rate.bits_per_second, after_up.rate.bits_per_second);
  EXPECT_EQ(old->authority_revision(), 10u);
  EXPECT_EQ(old->configured_encoder_kbps(), 8000);
}

TEST(StreamAppliedPacing, BackendSettingWithoutSuccessfulSubmissionCannotSupplyAFrameIdentity) {
  application::identity_source_t ids;
  application::adapter_t adapter {generation, application::backend_e::avcodec, ids};
  adapter.avcodec_open_success(application::origin_t {10, 8000}, {8000000, 8000000, 8000000, false},
    {8000000, 8000000, 8000000, false});
  ASSERT_TRUE(adapter.publish_usable_avcodec_open());
  // Even a successful open and request cap cannot replace a submitted output.
  EXPECT_FALSE(adapter.take_output(application::output_stage_e::avcodec_packet_pts, 7));
  EXPECT_EQ(select(nullptr).source, applied::source_e::frame_unavailable);
  {
    auto failed_submit = adapter.capture_before_submit(7);
  }
  EXPECT_FALSE(adapter.take_output(application::output_stage_e::avcodec_packet_pts, 7));
  auto submitted = adapter.capture_before_submit(8);
  ASSERT_TRUE(submitted.accepted(application::submit_stage_e::avcodec_send_frame_success));
  const auto output = adapter.take_output(application::output_stage_e::avcodec_packet_pts, 8);
  ASSERT_TRUE(output);
  EXPECT_EQ(output->authority_revision(), 10u);
  EXPECT_EQ(output->input_pts(), 8);
}

TEST(StreamAppliedPacing, PendingDownConstrictsAnOldFrameWithoutRestampingIt) {
  const auto old = frame(16000);
  ASSERT_TRUE(old);
  const auto before = select(&*old, cap_t {generation, 10, 16000});
  const auto down = select(&*old, cap_t {generation, 11, 4000});
  ASSERT_TRUE(down.applied());
  EXPECT_LT(down.rate.bits_per_second, before.rate.bits_per_second);
  EXPECT_GT(stream_pacing::send_offset(down.rate, 20), stream_pacing::send_offset(before.rate, 20));
  EXPECT_EQ(old->configured_encoder_kbps(), 16000);
  EXPECT_EQ(old->authority_revision(), 10u);
}

TEST(StreamAppliedPacing, ActiveFrameAndNextFrameKeepDebtAcrossDownThenUpAndProofLoss) {
  const auto old = frame(16000);
  ASSERT_TRUE(old);
  auto rate = select(&*old).rate;
  const auto original_due = stream_pacing::send_offset(rate, 20);
  const auto down = select(&*old, cap_t {generation, 12, 4000});
  applied::tighten(rate, down);
  const auto lower_due = stream_pacing::send_offset(rate, 20);
  EXPECT_GT(lower_due, original_due);
  applied::tighten(rate, select(&*old, cap_t {generation, 13, 16000}));
  applied::tighten(rate, select(&*old, std::nullopt));
  EXPECT_EQ(stream_pacing::send_offset(rate, 20), lower_due);
  stream_pacing::session_starts_t schedule;
  const auto start = std::chrono::steady_clock::time_point {10s};
  schedule.frame_end(generation, start + lower_due);
  EXPECT_EQ(schedule.frame_start(generation, start), start + lower_due);
  EXPECT_GT(start + stream_pacing::send_offset(rate, 21), start + lower_due);
}

TEST(StreamAppliedPacing, ConstructedMinimumParityAndPaddedTailConsumeTheWireCeiling) {
  const auto value = frame();
  ASSERT_TRUE(value);
  const auto selected = select(&*value, cap_t {generation, 11, 16000}, true, 32);
  ASSERT_TRUE(selected.applied());
  const auto &rate = selected.rate;
  // A constructed block has 3 data shards (including a padded tail) and 4
  // minimum parity shards. Nominal 10% would guess only one parity shard.
  constexpr std::size_t constructed_shards = 3 + 4;
  EXPECT_EQ(rate.bits_per_packet, (blocksize + 32) * 8);
  const auto exact_ns = constructed_shards * (blocksize + 32) * 8ULL * 1000000000ULL / rate.bits_per_second;
  EXPECT_EQ(stream_pacing::send_offset(rate, constructed_shards).count(), exact_ns);
  EXPECT_GT(stream_pacing::send_offset(rate, constructed_shards), stream_pacing::send_offset(rate, 3 + 1));
  EXPECT_GT(stream_pacing::send_offset(rate, constructed_shards), stream_pacing::send_offset(select(&*value).rate, constructed_shards));
}

TEST(StreamAppliedPacing, MultipleBlocksAndZeroParityChargeEveryActualDatagram) {
  const auto value = frame();
  ASSERT_TRUE(value);
  const auto rate = select(&*value).rate;
  // Real block objects supply these counts; the sender keeps a frame-wide
  // count instead of restarting debt at the next FEC block.
  constexpr std::size_t first = 200 + 20, second = 3 + 4, oversized_without_parity = 5;
  EXPECT_GT(stream_pacing::send_offset(rate, first + second), stream_pacing::send_offset(rate, first));
  const auto bits = (first + second + oversized_without_parity) * blocksize * 8ULL;
  EXPECT_EQ(stream_pacing::send_offset(rate, first + second + oversized_without_parity).count(),
    bits * 1000000000ULL / rate.bits_per_second);
}

TEST(StreamAppliedPacing, TinyAndHighlyPaddedFramesCannotInflateTheAdmittedRate) {
  const auto value = frame(16988);
  ASSERT_TRUE(value);
  const auto rate = select(&*value, cap_t {generation, 11, 20000}, true, 32).rate;
  // There is deliberately no encoded-frame-size input to rate selection.
  // A one-byte tail still costs a full packet; a repair-heavy tiny frame
  // takes more time under the same cap, never a faster nominal rate.
  for (const std::size_t tail_bytes : {1u, 2u, 1400u}) {
    EXPECT_GT(rate.bits_per_packet, tail_bytes * 8);
    EXPECT_LE(rate.bits_per_second, 18'988'000u);
  }
  EXPECT_GT(stream_pacing::send_offset(rate, 20), stream_pacing::send_offset(rate, 1));
  EXPECT_EQ(applied::packet_budget_kbps(request(), 2147483647), 18988);
  const auto low = applied::packet_budget_kbps(request(), 1000);
  ASSERT_TRUE(low);
  EXPECT_LT(*low, 18988);
}

TEST(StreamAppliedPacing, WrongGenerationPtsFutureRevisionAndInconsistentSameRevisionRefuse) {
  const auto wrong_owner = frame(8000, 10, 7, generation + 1);
  const auto wrong_pts = frame(8000, 10, 8);
  const auto future = frame(8000, 12);
  const auto value = frame();
  ASSERT_TRUE(wrong_owner);
  ASSERT_TRUE(wrong_pts);
  ASSERT_TRUE(future);
  ASSERT_TRUE(value);
  for (const auto *record : {&*wrong_owner, &*wrong_pts, &*future}) {
    EXPECT_EQ(select(record).source, applied::source_e::frame_mismatch);
  }
  EXPECT_EQ(select(&*value, cap_t {generation + 1, 11, 16000}).source, applied::source_e::frame_mismatch);
  EXPECT_EQ(select(&*value, cap_t {generation, 10, 16000}).source, applied::source_e::frame_mismatch);
}

TEST(StreamAppliedPacing, OffUnknownOwnerAndWatcherKeepTheExactCompatibilityRate) {
  const auto value = frame();
  ASSERT_TRUE(value);
  expect_same_rate(select(&*value, std::nullopt).rate, stream_pacing::select_rate(true, 20000, 100, blocksize));
  const auto off = select(&*value, cap_t {generation, 11, 16000}, false);
  EXPECT_EQ(off.source, applied::source_e::off);
  expect_same_rate(off.rate, stream_pacing::select_rate(false, 20000, 100, blocksize));
  auto watcher = request();
  watcher.split_kbps = 0;
  const auto refused = applied::select_rate(true, watcher, 100, blocksize, 0, generation, 7, &*value,
    adaptive_bitrate::live_pacing_cap_t {generation, 11, 16000});
  EXPECT_EQ(refused.source, applied::source_e::split_unavailable);
  expect_same_rate(refused.rate, stream_pacing::select_rate(true, 0, 100, blocksize));
  auto invalid = request();
  ++invalid.encoder_kbps;
  EXPECT_FALSE(applied::packet_budget_kbps(invalid, 8000));
  const auto invalid_cost = select(&*value, cap_t {generation, 11, 16000}, true, 65535);
  EXPECT_EQ(invalid_cost.source, applied::source_e::cost_unavailable);
  expect_same_rate(invalid_cost.rate, stream_pacing::select_rate(true, 20000, 100, blocksize));
  const auto unrepresentable = frame(1);
  ASSERT_TRUE(unrepresentable);
  const auto too_slow = select(&*unrepresentable, cap_t {generation, 10, 1});
  EXPECT_EQ(too_slow.source, applied::source_e::cost_unavailable);
  expect_same_rate(too_slow.rate, stream_pacing::select_rate(true, 20000, 100, blocksize));
}

namespace {
  class StreamAppliedPacingScope : public testing::Test {
    decltype(config::video.adaptive_bitrate) saved = config::video.adaptive_bitrate;
  protected:
    void SetUp() override {
      config::video.adaptive_bitrate.enabled = true;
      config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
      config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
      adaptive_bitrate::set_base_bitrate(16000);
      adaptive_bitrate::set_session_scope(generation, "pacing-owned", true);
      adaptive_bitrate::set_runtime_update_supported(true, {}, 16000);
    }
    void TearDown() override {
      adaptive_bitrate::set_session_scope(0, {}, false);
      config::video.adaptive_bitrate = saved;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
    }
  };
}

TEST_F(StreamAppliedPacingScope, ReadDoesNotPollAcknowledgeOrAdvanceTheController) {
  const auto before = adaptive_bitrate::get_state_with_history();
  const auto cap = adaptive_bitrate::get_live_pacing_cap(generation);
  ASSERT_TRUE(cap);
  EXPECT_EQ(cap->generation, generation);
  EXPECT_EQ(cap->target_encoder_kbps, 16000);
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(adaptive_bitrate::get_live_pacing_cap(generation));
  const auto after = adaptive_bitrate::get_state_with_history();
  EXPECT_EQ(after.history.entries.size(), before.history.entries.size());
  EXPECT_EQ(after.target_bitrate_kbps, before.target_bitrate_kbps);
  EXPECT_EQ(adaptive_bitrate::get_live_pacing_cap(generation)->revision, cap->revision);
  EXPECT_EQ(after.applied_bitrate_kbps, before.applied_bitrate_kbps);
}

TEST_F(StreamAppliedPacingScope, OtherOwnerOverlapEndAndUnsupportedNeverAdmit) {
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation + 1));
  adaptive_bitrate::set_session_scope(generation + 1, "second-owner", false);
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation));
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation + 1));
  adaptive_bitrate::set_session_scope(0, {}, false);
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation));
  adaptive_bitrate::set_session_scope(generation, "pacing-owned", true);
  adaptive_bitrate::set_runtime_update_supported(false);
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation));
}

TEST_F(StreamAppliedPacingScope, DisabledManualAndDoctorWritersKeepCompatibility) {
  adaptive_bitrate::set_runtime_enabled(false);
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation));
  adaptive_bitrate::set_runtime_enabled(true);
  adaptive_bitrate::set_live_bitrate_for_stream(8000);
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation));
  adaptive_bitrate::reset();
  adaptive_bitrate::set_base_bitrate(16000);
  adaptive_bitrate::set_session_scope(generation, "pacing-owned", true);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 16000);
  adaptive_bitrate::set_runtime_enabled(true);
  ASSERT_TRUE(adaptive_bitrate::get_live_pacing_cap(generation));
  const auto state = adaptive_bitrate::get_doctor_state();
  ASSERT_TRUE(adaptive_bitrate::set_doctor_bitrate_if_revision(state.revision, 8000));
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(generation));
}

TEST(StreamAppliedPacing, TrialAllocationUsesTheFramesPacketizerDomainAtLowHeadroom) {
  stream_bitrate::request_t budget;
  stream_bitrate::split_request(budget, 30000, 20, 512);
  ASSERT_EQ(budget.encoder_kbps, 22988);
  for (const int percentage : {40, 50}) {
    live_tuning_fec::actuator_t actuator;
    ASSERT_TRUE(actuator.configure(generation, budget));
    const auto trial = actuator.request(budget.encoder_kbps, percentage, 1, true, 10);
    ASSERT_TRUE(trial);
    ASSERT_EQ(actuator.confirm(generation, 10, *trial, 7, true, true),
      live_tuning_fec::actuator_t::confirmation_e::published);
    const auto allocation = actuator.allocation_for_frame(generation, 7, 77);
    ASSERT_EQ(allocation.first, 20);
    ASSERT_EQ(allocation.second, percentage);
    const auto value = frame(*trial);
    ASSERT_TRUE(value);
    const cap_t cap {generation, 10, *trial, budget.encoder_kbps};
    const auto selected = applied::select_rate(true, budget, 100, blocksize, 32,
      generation, 7, &*value, cap, allocation.second);
    ASSERT_TRUE(selected.applied());
    EXPECT_EQ(selected.rate.bits_per_second, 28'988'000u);
    EXPECT_EQ(selected.rate.bits_per_packet, (blocksize + 32) * 8);
    EXPECT_EQ(applied::packet_budget_kbps(budget, *trial), percentage == 40 ? 21488 : 17738);
    // Nominal policy units cannot masquerade as this revision's actual encoder setting.
    EXPECT_EQ(applied::select_rate(true, budget, 100, blocksize, 32, generation, 7, &*value,
      cap_t {generation, 10, budget.encoder_kbps, budget.encoder_kbps}, allocation.second).source,
      applied::source_e::frame_mismatch);
    EXPECT_EQ(value->configured_encoder_kbps(), *trial);
    EXPECT_EQ(value->authority_revision(), 10u);
    EXPECT_EQ(value->input_pts(), 7);
  }
}

TEST(StreamAppliedPacing, OldTrialRepairCannotInflateANewerLowerPolicyCeilingOrBorrowAnUp) {
  stream_bitrate::request_t budget;
  stream_bitrate::split_request(budget, 30000, 20, 512);
  live_tuning_fec::actuator_t actuator;
  ASSERT_TRUE(actuator.configure(generation, budget));
  const auto trial = actuator.request(16000, 40, 1, true, 10);
  ASSERT_TRUE(trial);
  ASSERT_EQ(actuator.confirm(generation, 10, *trial, 7, true, true),
    live_tuning_fec::actuator_t::confirmation_e::published);
  const int percentage = actuator.allocation_for_frame(generation, 7, 77).second;
  const auto old = frame(*trial);
  ASSERT_TRUE(old);
  const auto choose = [&](const cap_t &cap) {
    return applied::select_rate(true, budget, 100, blocksize, 32, generation, 7, &*old, cap, percentage);
  };
  const auto before = choose({generation, 10, *trial, 16000});
  const auto down = choose({generation, 11, 10000, 10000});
  const auto policy_budget = applied::packet_budget_kbps(budget, 10000);
  const auto wrong_domain = applied::packet_budget_kbps(budget, 10000, percentage);
  ASSERT_TRUE(before.applied());
  ASSERT_TRUE(down.applied());
  ASSERT_TRUE(policy_budget);
  ASSERT_TRUE(wrong_domain);
  EXPECT_GT(*wrong_domain, *policy_budget);
  EXPECT_EQ(down.rate.bits_per_second, *policy_budget * 1000);
  EXPECT_LT(down.rate.bits_per_second, before.rate.bits_per_second);
  const auto up = choose({generation, 12, budget.encoder_kbps, budget.encoder_kbps});
  ASSERT_TRUE(up.applied());
  expect_same_rate(up.rate, before.rate);
  auto active = before.rate;
  applied::tighten(active, down);
  const auto debt = stream_pacing::send_offset(active, 20);
  applied::tighten(active, up);
  EXPECT_EQ(stream_pacing::send_offset(active, 20), debt);
  EXPECT_GT(debt, stream_pacing::send_offset(before.rate, 20));
  EXPECT_EQ(old->configured_encoder_kbps(), *trial);
  EXPECT_EQ(old->authority_revision(), 10u);
  EXPECT_EQ(old->input_pts(), 7);
}

TEST_F(StreamAppliedPacingScope, CapReadIsUnavailableDuringSdkApplyAndActiveFrameKeepsDebt) {
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  const auto cap = adaptive_bitrate::get_live_pacing_cap(generation);
  ASSERT_TRUE(cap);
  EXPECT_EQ(cap->policy_encoder_kbps, cap->target_encoder_kbps);
  const auto value = frame(cap->target_encoder_kbps, cap->revision);
  ASSERT_TRUE(value);
  const auto before = select(&*value, cap);
  ASSERT_TRUE(before.applied());
  std::promise<void> entered, finish;
  auto resume = finish.get_future();
  auto update = std::async(std::launch::async, [&] {
    return adaptive_bitrate::apply_live_bitrate_request(*request, [&] {
      entered.set_value();
      resume.wait();
      return true;
    });
  });
  entered.get_future().wait();  // The SDK callback now owns state_mutex.
  auto reader = std::async(std::launch::async, [] {
    return adaptive_bitrate::get_live_pacing_cap(generation);
  });
  const auto status = reader.wait_for(100ms);
  // Release before assertions/join, so a blocking-read regression can fail cleanly.
  finish.set_value();
  const auto unavailable = reader.get();
  EXPECT_TRUE(update.get());
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_FALSE(unavailable);
  auto active = before.rate;
  const auto debt = stream_pacing::send_offset(active, 20);
  const auto candidate = select(&*value, unavailable);
  EXPECT_EQ(candidate.source, applied::source_e::owner_unavailable);
  applied::tighten(active, candidate);
  expect_same_rate(active, before.rate);
  EXPECT_EQ(stream_pacing::send_offset(active, 20), debt);
  stream_pacing::session_starts_t schedule;
  const auto start = std::chrono::steady_clock::time_point {10s};
  schedule.frame_end(generation, start + debt);
  EXPECT_EQ(schedule.frame_start(generation, start), start + debt);
  const auto after = adaptive_bitrate::get_live_pacing_cap(generation);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->generation, cap->generation);
  EXPECT_EQ(after->revision, cap->revision);
  EXPECT_EQ(after->target_encoder_kbps, cap->target_encoder_kbps);
  EXPECT_EQ(after->policy_encoder_kbps, cap->policy_encoder_kbps);
}

TEST(StreamAppliedPacing, PacketizerFitAndOversizedFallbackKeepTheAllocationRepairDomain) {
  stream_bitrate::request_t budget;
  stream_bitrate::split_request(budget, 30000, 20, 512);
  ASSERT_EQ(budget.encoder_kbps, 22988);
  constexpr std::size_t prefix = 32;
  // Packetized sizes where the production fit steps a 50% allocation down to
  // 35% or 30%, and one past the configured envelope (zero-parity fallback).
  constexpr std::size_t fits_35 = 4 * 188 * blocksize, fits_30 = 4 * 196 * blocksize,
                        oversized = 4 * 212 * blocksize + 1;
  struct case_t {
    int trial;
    std::size_t bytes;
    int packetized;
    std::int64_t packetized_budget;
  };
  for (const auto &c : {case_t {0, oversized, 0, 22988}, case_t {50, fits_35, 35, 22065},
                        case_t {50, fits_30, 30, 20417}, case_t {50, oversized, 0, 13988}}) {
    SCOPED_TRACE(testing::Message() << "trial " << c.trial << " bytes " << c.bytes);
    live_tuning_fec::actuator_t actuator;
    ASSERT_TRUE(actuator.configure(generation, budget));
    int encoder = budget.encoder_kbps;
    if (c.trial) {
      const auto trial = actuator.request(budget.encoder_kbps, c.trial, 1, true, 10);
      ASSERT_TRUE(trial);
      ASSERT_EQ(actuator.confirm(generation, 10, *trial, 7, true, true),
        live_tuning_fec::actuator_t::confirmation_e::published);
      encoder = *trial;
    }
    const auto allocation = actuator.allocation_for_frame(generation, 7, 77);
    ASSERT_EQ(allocation.first, 20);
    ASSERT_EQ(allocation.second, c.trial ? c.trial : 20);
    // The packetizer's own result: a fitted envelope, or zero parity for a
    // frame the configured envelope cannot protect (the sender's fallback).
    const auto fit = stream::fit_frame_fec(c.bytes, blocksize, allocation.first, allocation.second, 2);
    ASSERT_TRUE(fit);
    ASSERT_EQ(fit->fec_percentage, c.packetized);
    const auto value = frame(encoder);
    ASSERT_TRUE(value);
    const auto choose = [&](const cap_t &cap, int percentage) {
      return applied::select_rate(true, budget, 100, blocksize, prefix, generation, 7, &*value, cap, percentage);
    };
    const cap_t current {generation, 10, encoder, budget.encoder_kbps};
    const auto selected = choose(current, allocation.second);
    ASSERT_TRUE(selected.applied());
    EXPECT_EQ(selected.rate.bits_per_second, 28'988'000u);
    EXPECT_EQ(selected.rate.bits_per_packet, (blocksize + prefix) * 8);
    // Inverting through the packetized percentage under-admits this frame.
    EXPECT_EQ(applied::packet_budget_kbps(budget, encoder, c.packetized), c.packetized_budget);
    EXPECT_LT(choose(current, c.packetized).rate.bits_per_second, selected.rate.bits_per_second);
    // The current F0 policy ceiling still bounds the allocation's domain.
    const auto lower = choose({generation, 11, 10000, 10000}, allocation.second);
    ASSERT_TRUE(lower.applied());
    EXPECT_EQ(lower.rate.bits_per_second, 12'753'000u);
  }
}

TEST_F(StreamAppliedPacingScope, BusyBlockZeroStartCanOnlyTightenUnderALaterCoherentCap) {
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  const auto cap = adaptive_bitrate::get_live_pacing_cap(generation);
  ASSERT_TRUE(cap);
  const auto value = frame(cap->target_encoder_kbps, cap->revision);
  ASSERT_TRUE(value);
  const auto legacy = stream_pacing::select_rate(true, 20000, 100, blocksize);
  std::promise<void> entered, finish;
  auto owned = entered.get_future();
  auto resume = finish.get_future();
  auto update = std::async(std::launch::async, [&] {
    return adaptive_bitrate::apply_live_bitrate_request(*request, [&] {
      entered.set_value();
      resume.wait();
      return true;
    });
  });
  // Bounded, so a refusal before the SDK callback fails instead of hanging.
  const auto sdk = owned.wait_for(10s);
  auto block_zero = std::async(std::launch::async, [&] {
    return select(&*value, adaptive_bitrate::get_live_pacing_cap(generation));
  });
  const auto status = block_zero.wait_for(100ms);
  // Release before assertions/join, so a blocking-read regression can fail cleanly.
  finish.set_value();
  const auto start = block_zero.get();
  EXPECT_TRUE(update.get());
  ASSERT_EQ(sdk, std::future_status::ready);
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(start.source, applied::source_e::owner_unavailable);
  expect_same_rate(start.rate, legacy);
  ASSERT_TRUE(start.refreshable());
  auto active = start.rate;
  const auto legacy_due = stream_pacing::send_offset(active, 20);
  // The released owner's coherent cap tightens the frame from its original start.
  const auto later = select(&*value, adaptive_bitrate::get_live_pacing_cap(generation));
  ASSERT_TRUE(later.applied());
  applied::tighten(active, later);
  expect_same_rate(active, later.rate);
  const auto debt = stream_pacing::send_offset(active, 20);
  EXPECT_GT(debt, legacy_due);
  // Busy, missing or higher caps never speed it up again or forgive the debt.
  applied::tighten(active, start);
  applied::tighten(active, select(&*value, std::nullopt));
  applied::tighten(active, select(&*value, cap_t {generation, cap->revision + 1, 50000}));
  EXPECT_EQ(stream_pacing::send_offset(active, 20), debt);
  // Unknown, off, mismatched and unrepresentable starts stay legacy for the frame.
  EXPECT_FALSE(select(nullptr, cap).refreshable());
  EXPECT_FALSE(select(&*value, cap, false).refreshable());
  EXPECT_FALSE(select(&*value, cap_t {generation + 1, cap->revision, cap->target_encoder_kbps}).refreshable());
  EXPECT_FALSE(select(&*value, cap, true, 65535).refreshable());
  // Disabled and manual owners stay unreadable, so their frames keep the legacy rate.
  adaptive_bitrate::set_runtime_enabled(false);
  auto disabled = select(&*value, adaptive_bitrate::get_live_pacing_cap(generation));
  EXPECT_EQ(disabled.source, applied::source_e::owner_unavailable);
  applied::tighten(disabled.rate, select(&*value, adaptive_bitrate::get_live_pacing_cap(generation)));
  expect_same_rate(disabled.rate, legacy);
  adaptive_bitrate::set_runtime_enabled(true);
  adaptive_bitrate::set_live_bitrate_for_stream(8000);
  auto manual = select(&*value, adaptive_bitrate::get_live_pacing_cap(generation));
  EXPECT_EQ(manual.source, applied::source_e::owner_unavailable);
  applied::tighten(manual.rate, select(&*value, adaptive_bitrate::get_live_pacing_cap(generation)));
  expect_same_rate(manual.rate, legacy);
}
