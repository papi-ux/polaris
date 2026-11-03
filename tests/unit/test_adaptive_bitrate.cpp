/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Unit tests for adaptive bitrate controller behavior.
 */

#include "src/adaptive_bitrate.h"
#include "src/config.h"
#include "src/live_tuning_fec.h"
#include "src/video_bitrate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <future>
#include <limits>
#include <vector>
#ifdef __linux__
  #include <filesystem>
  #include <fstream>
  #include <cstdlib>
  #include <stdexcept>
  #include <nlohmann/json.hpp>
#endif

using namespace std::chrono_literals;

namespace {
  stream_bitrate::request_t fec_budget(int percentage = 10) {
    stream_bitrate::request_t request;
    stream_bitrate::split_request(request, 40000, percentage, 512);
    return request;
  }
}

TEST(LiveTuningFecActuator, MapsPolicyUnitsOnceAndRefusesAnUnprovenBudgetOrFloor) {
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  EXPECT_EQ(actuator.encoder_kbps(budget.encoder_kbps, 10), budget.encoder_kbps);
  const auto trial = actuator.encoder_kbps(budget.encoder_kbps, 15);
  ASSERT_TRUE(trial);
  EXPECT_LT(*trial, budget.encoder_kbps);
  EXPECT_LE(stream_bitrate::wire_kbps_for_encoder(*trial, 15, 512), budget.split_kbps);
  EXPECT_FALSE(actuator.encoder_kbps(budget.encoder_kbps + 1, 15));
  EXPECT_FALSE(actuator.request(budget.encoder_kbps, 15, *trial + 1, true, 7));
  EXPECT_FALSE(actuator.request(budget.encoder_kbps, 15, 2000, false, 7));
  EXPECT_FALSE(actuator.request(budget.encoder_kbps, 51, 2000, true, 7));
  EXPECT_FALSE(actuator.pending());
  for (const int fixed : {50, 255}) {
    EXPECT_FALSE(actuator.configure(1, fec_budget(fixed)));
  }
  auto watcher = budget;
  watcher.split_kbps = 0;
  EXPECT_FALSE(actuator.configure(1, watcher));
  auto mismatch = budget;
  ++mismatch.encoder_kbps;
  EXPECT_FALSE(actuator.configure(1, mismatch));
  EXPECT_FALSE(actuator.configure(0, budget));
}

TEST(LiveTuningFecActuator, PublishesOnlyAfterTheExactEncoderAckAtTheNextFrame) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
  ASSERT_TRUE(encoder);
  EXPECT_EQ(actuator.percentage_for_frame(1, 100, 0), 10);
  EXPECT_EQ(actuator.confirm(1, 7, *encoder + 1, 100, true, true), result_e::none);
  ASSERT_TRUE(actuator.pending());
  EXPECT_EQ(actuator.confirm(1, 7, *encoder, 100, true, true), result_e::published);
  EXPECT_EQ(actuator.percentage_for_frame(1, 99, 0), 10);
  EXPECT_EQ(actuator.percentage_for_frame(1, 100, 0), 15);
  EXPECT_EQ(actuator.percentage_for_frame(2, 100, 77), 77);
  EXPECT_EQ(actuator.confirm(1, 7, *encoder, 100, true, true), result_e::none);
  // Losing evidence or capability after publication holds the proven F.
  EXPECT_EQ(actuator.confirm(1, 7, *encoder, 101, false, false), result_e::none);
  EXPECT_EQ(actuator.percentage_for_frame(1, 101, 0), 15);
  for (const bool changed_generation : {false, true}) {
    SCOPED_TRACE(changed_generation);
    actuator.restore();
    ASSERT_TRUE(actuator.request(budget.encoder_kbps, 15, 2000, true, 9));
    EXPECT_EQ(actuator.confirm(changed_generation ? 2 : 1, changed_generation ? 9 : 10,
      *encoder, 102, true, true), result_e::stale);
    EXPECT_FALSE(actuator.pending());
    EXPECT_EQ(actuator.percentage_for_frame(1, 102, 0), 10);
  }
}

TEST(LiveTuningFecActuator, MissingFeedbackCapabilityOrFrameBoundaryCannotPublishARequest) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  const auto budget = fec_budget();
  for (const int refusal : {0, 1, 2}) {
    SCOPED_TRACE(refusal);
    live_tuning_fec::actuator_t actuator;
    ASSERT_TRUE(actuator.configure(1, budget));
    const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
    ASSERT_TRUE(encoder);
    EXPECT_EQ(actuator.confirm(1, 7, *encoder, refusal == 0 ? 0 : 100,
      refusal != 1, refusal != 2), result_e::refused);
    EXPECT_FALSE(actuator.pending());
    EXPECT_EQ(actuator.percentage_for_frame(1, 100, 0), 10);
    EXPECT_EQ(actuator.percentage(), 10);
  }
}

TEST(LiveTuningFecActuator, LowersParityBeforeRequestingTheHigherRateAndRejectsAnAbaAck) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
  ASSERT_TRUE(encoder);
  ASSERT_EQ(actuator.confirm(1, 7, *encoder, 100, true, true), result_e::published);
  const auto restore_rate = actuator.request(budget.encoder_kbps, 10,
    budget.encoder_kbps + 1, false, 8);  // the extra FEC floor never blocks F0
  ASSERT_EQ(restore_rate, budget.encoder_kbps);
  // Even queued older frames use lower parity before the caller can write the higher target.
  EXPECT_EQ(actuator.percentage_for_frame(1, 99, 0), 10);
  EXPECT_EQ(actuator.percentage_for_frame(1, 100, 0), 10);
  EXPECT_EQ(actuator.confirm(1, 8, *restore_rate, 101, true, true), result_e::restored);
  encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 9);
  ASSERT_TRUE(encoder);
  actuator.restore();  // outside writer, even one writing the very same kbps
  const auto new_request = actuator.request(budget.encoder_kbps, 15, 2000, true, 10);
  ASSERT_EQ(new_request, encoder);
  EXPECT_EQ(actuator.confirm(1, 9, *encoder, 102, true, true), result_e::stale);
  EXPECT_EQ(actuator.percentage_for_frame(1, 102, 0), 10);
  EXPECT_FALSE(actuator.pending());
  ASSERT_TRUE(actuator.request(budget.encoder_kbps, 15, 2000, true, 11));
  EXPECT_EQ(actuator.confirm(1, 11, *encoder, 103, true, true), result_e::published);
  actuator.reset();
  EXPECT_EQ(actuator.percentage_for_frame(1, 103, 77), 77);
}

TEST(LiveTuningFecActuator, PacketizerReadsOneCoherentGenerationAndPercentage) {
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  std::atomic<bool> done {false};
  std::atomic<bool> invalid {false};
  std::promise<void> first_read;
  auto started = first_read.get_future();
  std::thread reader([&] {
    const auto initial = actuator.allocation_for_frame(1, 100, 77);
    if (initial != std::pair {10, 10}) invalid.store(true);
    first_read.set_value();
    while (!done.load(std::memory_order_acquire)) {
      const auto [configured, requested] = actuator.allocation_for_frame(1, 100, 77);
      if (configured != 10 || (requested != 10 && requested != 15)) {
        invalid.store(true, std::memory_order_relaxed);
      }
    }
  });
  started.wait();
  bool writer_ok = true;
  for (std::uint64_t i = 1; i <= 1000; ++i) {
    const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, i);
    if (!encoder || actuator.confirm(1, i, *encoder, 100, true, true) !=
        live_tuning_fec::actuator_t::confirmation_e::published) {
      writer_ok = false;
      break;
    }
    actuator.restore();
  }
  done.store(true, std::memory_order_release);
  reader.join();
  EXPECT_TRUE(writer_ok);
  EXPECT_FALSE(invalid.load());
}

TEST(LiveTuningFecActuator, SelectionRetainsTheCheckedRevisionOnlyAtItsConfirmedFrameBoundary) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  using basis_e = live_tuning_fec::selection_basis_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
  ASSERT_TRUE(encoder);
  const auto pending = actuator.selection_for_frame(1, 100, 77);
  EXPECT_EQ(pending.allocation, (std::pair {10, 10}));
  EXPECT_EQ(pending.basis, basis_e::configured_baseline);
  EXPECT_EQ(pending.generation, 1u);
  EXPECT_FALSE(pending.authority_revision);
  EXPECT_FALSE(pending.from_frame);
  EXPECT_EQ(actuator.confirm(1, 7, *encoder + 1, 100, true, true), result_e::none);
  EXPECT_FALSE(actuator.selection_for_frame(1, 100, 77).authority_revision);
  ASSERT_EQ(actuator.confirm(1, 7, *encoder, 100, true, true), result_e::published);
  const auto old_frame = actuator.selection_for_frame(1, 99, 77);
  EXPECT_EQ(old_frame.allocation, (std::pair {10, 10}));
  EXPECT_EQ(old_frame.basis, basis_e::configured_baseline);
  EXPECT_EQ(old_frame.generation, 1u);
  EXPECT_FALSE(old_frame.authority_revision);
  EXPECT_FALSE(old_frame.from_frame);
  const auto selected = actuator.selection_for_frame(1, 100, 77);
  EXPECT_EQ(selected.allocation, (std::pair {10, 15}));
  EXPECT_EQ(selected.basis, basis_e::encoder_confirmed_trial);
  EXPECT_EQ(selected.generation, 1u);
  EXPECT_EQ(selected.authority_revision, 7u);
  EXPECT_EQ(selected.from_frame, 100);
  // A later callback cannot relabel the immutable, already confirmed publication.
  EXPECT_EQ(actuator.confirm(1, 8, *encoder, 101, false, false), result_e::none);
  const auto later = actuator.selection_for_frame(1, 101, 77);
  EXPECT_EQ(later.authority_revision, 7u);
  EXPECT_EQ(later.from_frame, 100);
}

TEST(LiveTuningFecActuator, RefusedOrStaleConfirmationNeverAddsTrialIdentity) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  using basis_e = live_tuning_fec::selection_basis_e;
  const auto budget = fec_budget();
  for (const int refusal : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(refusal);
    live_tuning_fec::actuator_t actuator;
    ASSERT_TRUE(actuator.configure(1, budget));
    const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
    ASSERT_TRUE(encoder);
    EXPECT_EQ(actuator.confirm(refusal == 3 ? 2 : 1, refusal == 4 ? 8 : 7,
      *encoder, refusal == 0 ? 0 : 100, refusal != 1, refusal != 2),
      refusal >= 3 ? result_e::stale : result_e::refused);
    EXPECT_FALSE(actuator.pending());
    const auto selected = actuator.selection_for_frame(1, 100, 77);
    EXPECT_EQ(selected.allocation, (std::pair {10, 10}));
    EXPECT_EQ(selected.basis, basis_e::configured_baseline);
    EXPECT_EQ(selected.generation, 1u);
    EXPECT_FALSE(selected.authority_revision);
    EXPECT_FALSE(selected.from_frame);
    // Refusal is not sticky identity: only a fresh, exactly confirmed request can supply it.
    ASSERT_TRUE(actuator.request(budget.encoder_kbps, 15, 2000, true, 9));
    ASSERT_EQ(actuator.confirm(1, 9, *encoder, 101, true, true), result_e::published);
    const auto admitted = actuator.selection_for_frame(1, 101, 77);
    EXPECT_EQ(admitted.basis, basis_e::encoder_confirmed_trial);
    EXPECT_EQ(admitted.authority_revision, 9u);
    EXPECT_EQ(admitted.from_frame, 101);
  }
}

TEST(LiveTuningFecActuator, SamePercentageAbaCannotBorrowEitherRequestsTrialIdentity) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  using basis_e = live_tuning_fec::selection_basis_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  const auto first = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
  ASSERT_TRUE(first);
  actuator.restore();
  const auto second = actuator.request(budget.encoder_kbps, 15, 2000, true, 9);
  ASSERT_EQ(first, second);  // Same percentage and bitrate, distinct checked authority.
  EXPECT_EQ(actuator.confirm(1, 7, *first, 100, true, true), result_e::stale);
  const auto stale = actuator.selection_for_frame(1, 100, 77);
  EXPECT_EQ(stale.allocation, (std::pair {10, 10}));
  EXPECT_EQ(stale.basis, basis_e::configured_baseline);
  EXPECT_FALSE(stale.authority_revision);
  EXPECT_FALSE(stale.from_frame);
  EXPECT_EQ(actuator.confirm(1, 9, *second, 101, true, true), result_e::none);
  EXPECT_FALSE(actuator.selection_for_frame(1, 101, 77).authority_revision);
  ASSERT_TRUE(actuator.request(budget.encoder_kbps, 15, 2000, true, 11));
  ASSERT_EQ(actuator.confirm(1, 11, *first, 102, true, true), result_e::published);
  const auto selected = actuator.selection_for_frame(1, 102, 77);
  EXPECT_EQ(selected.allocation, (std::pair {10, 15}));
  EXPECT_EQ(selected.authority_revision, 11u);
  EXPECT_EQ(selected.from_frame, 102);
}

TEST(LiveTuningFecActuator, RestoreAndReconfigureExposeOnlyConfiguredBaselineScope) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  using basis_e = live_tuning_fec::selection_basis_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
  ASSERT_TRUE(encoder);
  ASSERT_EQ(actuator.confirm(1, 7, *encoder, 100, true, true), result_e::published);
  const auto selected = actuator.selection_for_frame(1, 100, 77);
  const auto restored_rate = actuator.request(budget.encoder_kbps, 10, 2000, false, 8);
  ASSERT_TRUE(restored_rate);
  for (const bool confirmed : {false, true}) {
    if (confirmed) {
      ASSERT_EQ(actuator.confirm(1, 8, *restored_rate, 101, true, true), result_e::restored);
    }
    const auto baseline = actuator.selection_for_frame(1, 100, 77);
    EXPECT_EQ(baseline.allocation, (std::pair {10, 10}));
    EXPECT_EQ(baseline.basis, basis_e::configured_baseline);
    EXPECT_EQ(baseline.generation, 1u);
    EXPECT_FALSE(baseline.authority_revision);
    EXPECT_FALSE(baseline.from_frame);
  }
  // The previously selected frame keeps the tuple it obtained before restore.
  EXPECT_EQ(selected.allocation, (std::pair {10, 15}));
  EXPECT_EQ(selected.basis, basis_e::encoder_confirmed_trial);
  EXPECT_EQ(selected.authority_revision, 7u);
  EXPECT_EQ(selected.from_frame, 100);
  // Reusing a configured scope does not manufacture a baseline publication epoch.
  ASSERT_TRUE(actuator.configure(1, fec_budget(0)));
  const auto zero_baseline = actuator.selection_for_frame(1, 102, 77);
  EXPECT_EQ(zero_baseline.allocation, (std::pair {0, 0}));
  EXPECT_EQ(zero_baseline.basis, basis_e::configured_baseline);
  EXPECT_EQ(zero_baseline.generation, 1u);
  EXPECT_FALSE(zero_baseline.authority_revision);
  EXPECT_FALSE(zero_baseline.from_frame);
}

TEST(LiveTuningFecActuator, InvalidFramesAndForeignGenerationsKeepLegacyValuesWithoutIdentity) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  using basis_e = live_tuning_fec::selection_basis_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  const auto encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 7);
  ASSERT_TRUE(encoder);
  ASSERT_EQ(actuator.confirm(1, 7, *encoder, 100, true, true), result_e::published);
  for (const std::int64_t frame : {std::numeric_limits<std::int64_t>::min(),
       std::int64_t {-1}, std::int64_t {0}, std::int64_t {99}, std::int64_t {100},
       std::numeric_limits<std::int64_t>::max()}) {
    for (const std::uint64_t generation : {0u, 1u, 2u}) {
      SCOPED_TRACE(frame);
      SCOPED_TRACE(generation);
      const auto expected = generation == 1 ? std::pair {10, frame >= 100 ? 15 : 10} : std::pair {77, 77};
      const auto selected = actuator.selection_for_frame(generation, frame, 77);
      EXPECT_EQ(selected.allocation, expected);
      EXPECT_EQ(actuator.allocation_for_frame(generation, frame, 77), expected);
      EXPECT_EQ(actuator.percentage_for_frame(generation, frame, 77), expected.second);
      if (generation != 1 || frame <= 0) {
        EXPECT_EQ(selected.basis, basis_e::unavailable);
        EXPECT_FALSE(selected.generation);
        EXPECT_FALSE(selected.authority_revision);
        EXPECT_FALSE(selected.from_frame);
      } else {
        EXPECT_EQ(selected.generation, 1u);
        EXPECT_EQ(selected.basis, frame < 100 ? basis_e::configured_baseline : basis_e::encoder_confirmed_trial);
      }
    }
  }
  ASSERT_TRUE(actuator.configure(2, fec_budget(20)));
  const auto foreign = actuator.selection_for_frame(1, 100, 77);
  EXPECT_EQ(foreign.allocation, (std::pair {77, 77}));
  EXPECT_EQ(foreign.basis, basis_e::unavailable);
  EXPECT_FALSE(foreign.generation);
  EXPECT_FALSE(foreign.authority_revision);
  actuator.reset();
  const auto absent = actuator.selection_for_frame(2, 100, 77);
  EXPECT_EQ(absent.allocation, (std::pair {77, 77}));
  EXPECT_EQ(absent.basis, basis_e::unavailable);
  EXPECT_FALSE(absent.generation);
  EXPECT_FALSE(absent.authority_revision);
  EXPECT_FALSE(absent.from_frame);
}

TEST(LiveTuningFecActuator, ConcurrentSelectionKeepsAllocationRevisionAndBoundaryInOneTuple) {
  using result_e = live_tuning_fec::actuator_t::confirmation_e;
  using basis_e = live_tuning_fec::selection_basis_e;
  live_tuning_fec::actuator_t actuator;
  const auto budget = fec_budget();
  ASSERT_TRUE(actuator.configure(1, budget));
  const auto initial_encoder = actuator.request(budget.encoder_kbps, 15, 2000, true, 1);
  ASSERT_TRUE(initial_encoder);
  ASSERT_EQ(actuator.confirm(1, 1, *initial_encoder, 101, true, true), result_e::published);
  std::atomic<bool> done {false};
  std::atomic<bool> invalid {false};
  live_tuning_fec::selection_t first_selected {};
  live_tuning_fec::selection_t last_selected {};
  std::promise<void> first_read;
  auto started = first_read.get_future();
  std::thread reader([&] {
    const auto valid = [](const live_tuning_fec::selection_t &selected) {
      if (selected.basis == basis_e::unavailable) {
        return selected.allocation == std::pair {77, 77} && !selected.generation &&
          !selected.authority_revision && !selected.from_frame;
      }
      if (selected.generation != 1u || selected.allocation.first != 10) return false;
      if (selected.basis == basis_e::configured_baseline) {
        return selected.allocation.second == 10 && !selected.authority_revision && !selected.from_frame;
      }
      if (!selected.authority_revision || *selected.authority_revision < 1 ||
          *selected.authority_revision > 1000 || !selected.from_frame) return false;
      const auto revision = *selected.authority_revision;
      return selected.basis == basis_e::encoder_confirmed_trial &&
        selected.allocation.second == (revision % 2 ? 15 : 20) &&
        *selected.from_frame == 100 + static_cast<std::int64_t>(revision);
    };
    first_selected = actuator.selection_for_frame(1, 5000, 77);
    if (!valid(first_selected)) invalid.store(true);
    first_read.set_value();
    while (!done.load(std::memory_order_acquire)) {
      if (!valid(actuator.selection_for_frame(1, 5000, 77))) {
        invalid.store(true, std::memory_order_relaxed);
      }
    }
    last_selected = actuator.selection_for_frame(1, 5000, 77);
    if (!valid(last_selected)) invalid.store(true);
  });
  // Bound test-thread startup only; this is not an actuator or product deadline.
  const auto ready = started.wait_for(10s);
  bool writer_ok = ready == std::future_status::ready;
  actuator.restore();
  for (std::uint64_t revision = 2; writer_ok && revision <= 1000; ++revision) {
    const auto encoder = actuator.request(budget.encoder_kbps, revision % 2 ? 15 : 20, 2000, true, revision);
    if (!encoder || actuator.confirm(1, revision, *encoder, 100 + static_cast<std::int64_t>(revision), true, true) !=
        result_e::published) {
      writer_ok = false;
      break;
    }
    actuator.restore();
    if (!actuator.configure(2, fec_budget(20)) || !actuator.configure(1, budget)) {
      writer_ok = false;
      break;
    }
  }
  actuator.restore();
  done.store(true, std::memory_order_release);
  reader.join();
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(writer_ok);
  EXPECT_FALSE(invalid.load());
  // The reader must witness an actual trial and then the restored baseline;
  // a baseline/fallback-only schedule cannot satisfy this concurrent guard.
  EXPECT_EQ(first_selected.allocation, (std::pair {10, 15}));
  EXPECT_EQ(first_selected.basis, basis_e::encoder_confirmed_trial);
  EXPECT_EQ(first_selected.generation, 1u);
  EXPECT_EQ(first_selected.authority_revision, 1u);
  EXPECT_EQ(first_selected.from_frame, 101);
  EXPECT_EQ(last_selected.allocation, (std::pair {10, 10}));
  EXPECT_EQ(last_selected.basis, basis_e::configured_baseline);
  EXPECT_EQ(last_selected.generation, 1u);
  EXPECT_FALSE(last_selected.authority_revision);
  EXPECT_FALSE(last_selected.from_frame);
}

namespace {
  void enable_controller(int base_bitrate_kbps = 20000) {
    config::video.adaptive_bitrate.enabled = true;
    config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
    config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    adaptive_bitrate::set_runtime_update_supported(true);
    adaptive_bitrate::set_base_bitrate(base_bitrate_kbps);
  }

  /// Client media reports of 120 frames each, losing loss_pct of them. Three are what Live Tuning's policy
  /// needs to start a probe while its baseline warms up; the probe's first step is 0.6x.
  void sustain_media_loss(double loss_pct, int reports = 3, double rtt_ms = 8.0) {
    for (int i = 0; i < reports; ++i) {
      adaptive_bitrate::update_media_report(120.0, std::round(120.0 * loss_pct / 100.0), rtt_ms);
    }
  }
}

TEST(AdaptiveBitrateFecScope, RequiresTheTrustedSoleScopeAndKeepsRecreatingEncodersFixed) {
  const auto budget = fec_budget();
  config::video.adaptive_bitrate.enabled = true;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_base_bitrate(budget.encoder_kbps);
  adaptive_bitrate::set_session_scope(4931, "fec-owned", true);
  adaptive_bitrate::set_runtime_update_supported(true, {}, budget.encoder_kbps);
  adaptive_bitrate::configure_fec_budget(4932, budget, true);
  EXPECT_FALSE(adaptive_bitrate::get_state().fec_budget_available);
  adaptive_bitrate::configure_fec_budget(4931, budget, false);
  EXPECT_FALSE(adaptive_bitrate::get_state().fec_budget_available);
  adaptive_bitrate::configure_fec_budget(4931, budget, true);
  EXPECT_TRUE(adaptive_bitrate::get_state().fec_budget_available);
  EXPECT_FALSE(adaptive_bitrate::get_state().runtime_bitrate_in_place);
  adaptive_bitrate::note_fec_partial_deficit(4931, std::chrono::steady_clock::now());
  for (int i = 0; i < 100; ++i) adaptive_bitrate::update_media_report(120, 8, 3.0);
  EXPECT_EQ(adaptive_bitrate::get_state().current_fec_percentage, 10);
  const auto history = adaptive_bitrate::get_state_with_history().history.entries;
  EXPECT_TRUE(std::any_of(history.begin(), history.end(), [](const auto &entry) {
    return entry.kind == "fec_unavailable" && entry.cause == "in_place_scope_unavailable";
  }));  // proves the policy asked; this is not a never-entered negative path
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4931, 100, 77), (std::pair {10, 10}));
  adaptive_bitrate::set_live_bitrate(25000);
  EXPECT_EQ(adaptive_bitrate::get_state().target_bitrate_kbps, 25000);
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4931, 100, 77), (std::pair {10, 10}));
  adaptive_bitrate::set_session_scope(4932, "shared", false);
  EXPECT_FALSE(adaptive_bitrate::get_state().fec_budget_available);
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4931, 100, 77), (std::pair {77, 77}));
  adaptive_bitrate::reset();
  adaptive_bitrate::set_session_scope(0, {}, false);
}

namespace {
  struct encoder_open_scope_t {
    encoder_open_scope_t() {
      enable_controller();
      adaptive_bitrate::set_runtime_update_supported(false, "not_open_yet");
      adaptive_bitrate::set_session_scope(605, "opening", true);
    }
    ~encoder_open_scope_t() {
      adaptive_bitrate::reset();
      adaptive_bitrate::set_session_scope(0, {}, false);
    }
  };
}

TEST(AdaptiveBitrateController, SnapshotsTheFirstOpenWithoutAcknowledgingApplication) {
  encoder_open_scope_t scope;
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto history = adaptive_bitrate::get_state_with_history().history.entries;
  for (int i = 0; i < 3; ++i) {
    const auto origin = adaptive_bitrate::get_encoder_open_request(605, 20000);
    ASSERT_TRUE(origin);
    EXPECT_EQ(origin->target_bitrate_kbps, 20000);
    EXPECT_EQ(origin->revision, before.revision);
  }
  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_FALSE(after.runtime_update_supported);
  EXPECT_EQ(after.revision, before.revision);
  EXPECT_EQ(after.action_authority_revision, before.action_authority_revision);
  EXPECT_EQ(after.live_bitrate_kbps, before.live_bitrate_kbps);
  EXPECT_EQ(adaptive_bitrate::get_state_with_history().history.entries.size(), history.size());
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 0);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, RefusesAnUnownedOrMismatchedOpenWithoutBorrowingSupport) {
  encoder_open_scope_t scope;
  EXPECT_FALSE(adaptive_bitrate::get_encoder_open_request(0, 20000));
  EXPECT_FALSE(adaptive_bitrate::get_encoder_open_request(606, 20000));
  EXPECT_FALSE(adaptive_bitrate::get_encoder_open_request(605, 0));
  EXPECT_FALSE(adaptive_bitrate::get_encoder_open_request(605, 10000));
  adaptive_bitrate::set_session_scope(605, "shared", false);
  EXPECT_FALSE(adaptive_bitrate::get_encoder_open_request(605, 20000));
  adaptive_bitrate::set_session_scope(605, "opening", true);
  ASSERT_TRUE(adaptive_bitrate::get_encoder_open_request(605, 20000));
  adaptive_bitrate::set_enabled(false);
  // Historical open attribution is separate from permission to pace a frame.
  EXPECT_TRUE(adaptive_bitrate::get_encoder_open_request(605, 20000));
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(605));
}

TEST(AdaptiveBitrateController, PendingRecreationCanObserveItsExactLowerOpenTarget) {
  encoder_open_scope_t scope;
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(10000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_EQ(request->target_bitrate_kbps, 10000);
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(request->revision, 10000));
  EXPECT_FALSE(adaptive_bitrate::get_live_pacing_cap(605));
  const auto origin = adaptive_bitrate::get_encoder_open_request(605, 10000);
  ASSERT_TRUE(origin);
  EXPECT_EQ(origin->revision, request->revision);
  EXPECT_EQ(origin->target_bitrate_kbps, 10000);
  EXPECT_FALSE(adaptive_bitrate::get_encoder_open_request(605, 20000));
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 0);
}

TEST(AdaptiveBitrateController, ReducesTargetOnNetworkPressure) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  sustain_media_loss(8.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_EQ(state.target_bitrate_kbps, 12000);
  EXPECT_EQ("network_pressure", state.state);
  EXPECT_EQ("packet_loss", state.reason);
}

TEST(AdaptiveBitrateFecScope, ARealPolicyEpisodePublishesAfterAckAndRetiresBeforeAnOutsideWriter) {
  struct encoder_t {
    enum class bitrate_update_e { rejected, applied, recreate_session };
    bitrate_update_e update_bitrate(int) { return bitrate_update_e::applied; }
  } encoder;
  const auto budget = fec_budget();
  enable_controller(budget.encoder_kbps);
  adaptive_bitrate::set_session_scope(4933, "fec-transaction", true);
  adaptive_bitrate::configure_fec_budget(4933, budget, true);
  adaptive_bitrate::set_runtime_update_supported(true, {}, budget.encoder_kbps, true, 1);
  int actual_encoder = budget.encoder_kbps;
  std::int64_t publication_frame = 0;
  std::optional<adaptive_bitrate::live_bitrate_request_t> last_request;
  for (int i = 1; i <= 100 && !publication_frame; ++i) {
    // Deliberately older than the former 2.5 s instant gate, still recent
    // selective evidence. Every new quantified media report is live.
    adaptive_bitrate::note_fec_partial_deficit(4933, std::chrono::steady_clock::now() - 5s);
    adaptive_bitrate::update_media_report(120, 8, 3.0);
    const auto request = adaptive_bitrate::get_live_bitrate_request();
    ASSERT_TRUE(request);
    const std::int64_t next_frame = 200 + i;
    const auto result = video::bitrate::apply_after_encode(request, encoder, actual_encoder,
      next_frame, false, [](const auto &...) {});
    ASSERT_TRUE(result.result == video::bitrate::result_e::applied ||
                result.result == video::bitrate::result_e::unchanged);
    last_request = request;
    if (adaptive_bitrate::get_state().current_fec_percentage == 15) publication_frame = next_frame;
  }
  ASSERT_GT(publication_frame, 0);  // cannot pass unless the full controller raise actually happened
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4933, publication_frame - 1, 77), (std::pair {10, 10}));
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4933, publication_frame, 77), (std::pair {10, 15}));
  EXPECT_LT(actual_encoder, adaptive_bitrate::get_state().policy_target_bitrate_kbps);
  adaptive_bitrate::set_live_bitrate(budget.encoder_kbps);
  const auto writer_history = adaptive_bitrate::get_state_with_history().history;
  const auto retired = std::find_if(writer_history.entries.begin(), writer_history.entries.end(), [](const auto &entry) {
    return entry.kind == "fec_retired" && entry.cause == "explicit_live_bitrate";
  });
  ASSERT_NE(retired, writer_history.entries.end());
  ASSERT_TRUE(retired->fec_transition);
  const auto &retirement = *retired->fec_transition;
  EXPECT_EQ(retirement.configured_percentage, 10);
  EXPECT_EQ(retirement.percentage_before, 15);
  EXPECT_EQ(retirement.percentage_after, 10);
  EXPECT_EQ(retirement.phase_before, "fixed");
  EXPECT_EQ(retirement.phase_after, "fixed");
  EXPECT_FALSE(retirement.pending_percentage_before);
  EXPECT_FALSE(retirement.pending_percentage_after);
  EXPECT_FALSE(retirement.from_frame_index);
  EXPECT_EQ(retired->requested_bitrate_kbps, actual_encoder);  // recorded before the writer's target change
  EXPECT_TRUE(retired->applied_known);
  EXPECT_EQ(retired->applied_bitrate_kbps, actual_encoder);
  EXPECT_EQ(retired->repeat_count, 1u);
  EXPECT_EQ(std::count_if(writer_history.entries.begin(), writer_history.entries.end(), [](const auto &entry) {
    return entry.kind == "fec_retired";
  }), 1);
  EXPECT_TRUE(std::none_of(writer_history.entries.begin(), writer_history.entries.end(), [](const auto &entry) {
    return entry.kind == "fec_restored";
  }));
  // The higher target has not reached the emulated encoder yet: parity is
  // already back at F0 for both queued and future frames.
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4933, publication_frame - 1, 77), (std::pair {10, 10}));
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4933, publication_frame, 77), (std::pair {10, 10}));
  bool stale_sdk_called = false;
  ASSERT_TRUE(last_request);
  EXPECT_FALSE(adaptive_bitrate::apply_live_bitrate_request(*last_request,
    [&] { stale_sdk_called = true; return true; }, nullptr, publication_frame + 1));
  EXPECT_FALSE(stale_sdk_called);
  // Reuse this real public policy/ACK prefix, stopping before its trial's lower-rate ACK.
  // The two continuations distinguish an explicit writer from an actual new loss-policy decision.
  for (const bool automatic : {false, true}) {
    SCOPED_TRACE(automatic ? "policy cancellation" : "explicit cancellation");
    enable_controller(budget.encoder_kbps);
    adaptive_bitrate::set_session_scope(4933, automatic ? "fec-policy-cancel" : "fec-explicit-cancel", true);
    adaptive_bitrate::configure_fec_budget(4933, budget, true);
    adaptive_bitrate::set_runtime_update_supported(true, {}, budget.encoder_kbps, true, 1);
    actual_encoder = budget.encoder_kbps;
    std::optional<adaptive_bitrate::live_bitrate_request_t> unacknowledged;
    for (int i = 1; i <= 100 && !unacknowledged; ++i) {
      adaptive_bitrate::note_fec_partial_deficit(4933, std::chrono::steady_clock::now() - 5s);
      adaptive_bitrate::update_media_report(120, 8, 3.0);
      const auto next = adaptive_bitrate::get_live_bitrate_request();
      ASSERT_TRUE(next);
      if (adaptive_bitrate::get_state().fec_phase == "lowering_rate") {
        unacknowledged = next;
        break;
      }
      if (next->target_bitrate_kbps != actual_encoder) {
        ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*next,
          [] { return true; }, nullptr, 500 + i));
        actual_encoder = next->target_bitrate_kbps;
      } else {
        adaptive_bitrate::acknowledge_live_bitrate_applied(next->revision, actual_encoder, 500 + i);
      }
    }
    ASSERT_TRUE(unacknowledged);
    const auto pending = adaptive_bitrate::get_state_with_history();
    ASSERT_EQ(pending.fec_phase, "lowering_rate");
    ASSERT_EQ(pending.current_fec_percentage, 10);
    ASSERT_LT(unacknowledged->target_bitrate_kbps, pending.applied_bitrate_kbps);
    const auto trial = std::find_if(pending.history.entries.begin(), pending.history.entries.end(), [](const auto &entry) {
      return entry.kind == "fec_target" && entry.cause == "parity_trial";
    });
    ASSERT_NE(trial, pending.history.entries.end());
    ASSERT_TRUE(trial->fec_transition);
    ASSERT_EQ(trial->fec_transition->percentage_after, 10);
    ASSERT_EQ(trial->fec_transition->phase_after, "lowering_rate");
    ASSERT_EQ(trial->fec_transition->pending_percentage_after, 15);
    ASSERT_FALSE(trial->fec_transition->from_frame_index);
    ASSERT_TRUE(std::none_of(pending.history.entries.begin(), pending.history.entries.end(), [](const auto &entry) {
      return entry.kind == "fec_published";
    }));
    if (automatic) {
      // Fixed heavy reports start a real loss descent; no writer or lower-rate ACK substitutes for it.
      for (int report = 0; report < 32 && adaptive_bitrate::get_state().fec_phase == "lowering_rate"; ++report) {
        adaptive_bitrate::update_media_report(120, 120, 3.0);
      }
    } else {
      adaptive_bitrate::set_live_bitrate(budget.encoder_kbps);
    }
    const auto cancelled = adaptive_bitrate::get_state_with_history();
    EXPECT_EQ(cancelled.fec_phase, "fixed");
    EXPECT_EQ(cancelled.current_fec_percentage, 10);
    const auto event = std::find_if(cancelled.history.entries.begin(), cancelled.history.entries.end(), [&](const auto &entry) {
      return entry.kind == "fec_cancelled" && entry.cause ==
        (automatic ? "policy_target_changed" : "explicit_live_bitrate");
    });
    ASSERT_NE(event, cancelled.history.entries.end());
    ASSERT_TRUE(event->fec_transition);
    EXPECT_EQ(event->fec_transition->configured_percentage, 10);
    EXPECT_EQ(event->fec_transition->percentage_before, 10);
    EXPECT_EQ(event->fec_transition->percentage_after, 10);
    EXPECT_EQ(event->fec_transition->phase_before, "lowering_rate");
    EXPECT_EQ(event->fec_transition->phase_after, "fixed");
    EXPECT_EQ(event->fec_transition->pending_percentage_before, 15);
    EXPECT_FALSE(event->fec_transition->pending_percentage_after);
    EXPECT_FALSE(event->fec_transition->from_frame_index);
    EXPECT_GT(event->sequence, trial->sequence);
    EXPECT_EQ(event->repeat_count, 1u);
    EXPECT_EQ(std::count_if(cancelled.history.entries.begin(), cancelled.history.entries.end(), [](const auto &entry) {
      return entry.kind == "fec_cancelled";
    }), 1);
    EXPECT_TRUE(std::none_of(cancelled.history.entries.begin(), cancelled.history.entries.end(), [](const auto &entry) {
      return entry.kind == "fec_published" || entry.kind == "fec_retired" || entry.kind == "fec_restored";
    }));
  }
  adaptive_bitrate::reset();
  adaptive_bitrate::set_session_scope(0, {}, false);
}

TEST(AdaptiveBitrateFecScope, AnUnhelpedTrialRestoresConfiguredParityBeforeItsHigherRateAck) {
  const auto budget = fec_budget();
  enable_controller(budget.encoder_kbps);
  adaptive_bitrate::set_session_scope(4934, "fec-named-restore", true);
  adaptive_bitrate::configure_fec_budget(4934, budget, true);
  adaptive_bitrate::set_runtime_update_supported(true, {}, budget.encoder_kbps, true, 1);
  int actual_encoder = budget.encoder_kbps;
  std::int64_t publication_frame = 0;
  // The same real policy episode as above: its rate-independent result asks for 15, and the
  // lowered rate's exact ACK publishes it.
  for (int i = 1; i <= 100 && !publication_frame; ++i) {
    adaptive_bitrate::note_fec_partial_deficit(4934, std::chrono::steady_clock::now() - 5s);
    adaptive_bitrate::update_media_report(120, 8, 3.0);
    const auto request = adaptive_bitrate::get_live_bitrate_request();
    ASSERT_TRUE(request);
    const std::int64_t next_frame = 200 + i;
    if (request->target_bitrate_kbps != actual_encoder) {
      ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*request,
        [] { return true; }, nullptr, next_frame));
      actual_encoder = request->target_bitrate_kbps;
    } else {
      adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, actual_encoder, next_frame);
    }
    if (adaptive_bitrate::get_state().current_fec_percentage == 15) publication_frame = next_frame;
  }
  ASSERT_GT(publication_frame, 0);  // cannot pass unless the full controller raise actually happened
  ASSERT_EQ(adaptive_bitrate::frame_fec_allocation(4934, publication_frame, 77), (std::pair {10, 15}));
  const auto published_history = adaptive_bitrate::get_state_with_history().history;
  const auto published = std::find_if(published_history.entries.begin(), published_history.entries.end(), [](const auto &entry) {
    return entry.kind == "fec_published";
  });
  ASSERT_NE(published, published_history.entries.end());
  ASSERT_TRUE(published->fec_transition);
  EXPECT_EQ(published->fec_transition->configured_percentage, 10);
  EXPECT_EQ(published->fec_transition->percentage_before, 10);
  EXPECT_EQ(published->fec_transition->percentage_after, 15);
  EXPECT_EQ(published->fec_transition->phase_before, "lowering_rate");
  EXPECT_EQ(published->fec_transition->phase_after, "fixed");
  EXPECT_EQ(published->fec_transition->pending_percentage_before, 15);
  EXPECT_FALSE(published->fec_transition->pending_percentage_after);
  EXPECT_EQ(published->fec_transition->from_frame_index, publication_frame);
  EXPECT_EQ(published->callback_bitrate_kbps, actual_encoder);
  EXPECT_EQ(published->callback_revision, published->applied_revision);
  EXPECT_GT(published->callback_revision, 0u);
  const int trial_encoder = actual_encoder;
  ASSERT_EQ(adaptive_bitrate::get_state().policy_target_bitrate_kbps, budget.encoder_kbps);
  ASSERT_LT(trial_encoder, budget.encoder_kbps);

  // Post-FEC reports at the learned background loss: the parity did not help. The first report
  // after the raise is not judged, and the policy's "did it help" test decides within its bounded
  // report count, an undecided test counting as no help. Until then nothing moves the trial.
  std::optional<adaptive_bitrate::history_entry_t> restore;
  std::int64_t last_frame = publication_frame;
  for (int i = 1; i <= 32; ++i) {
    adaptive_bitrate::update_media_report(120, 8, 3.0);
    const auto entries = adaptive_bitrate::get_state_with_history().history.entries;
    const auto named = std::find_if(entries.begin(), entries.end(), [](const auto &entry) {
      return entry.kind == "fec_target" && entry.cause == "restore_configured_parity";
    });
    if (named != entries.end()) {
      restore = *named;
      break;
    }
    ASSERT_EQ(adaptive_bitrate::get_state().current_fec_percentage, 15);
    const auto request = adaptive_bitrate::get_live_bitrate_request();
    ASSERT_TRUE(request);
    ASSERT_EQ(request->target_bitrate_kbps, trial_encoder);
    last_frame = publication_frame + i;
    adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, trial_encoder, last_frame);
  }
  ASSERT_TRUE(restore);  // the policy itself asked for the configured parity; no writer retired it
  ASSERT_TRUE(restore->fec_transition);
  EXPECT_EQ(restore->fec_transition->configured_percentage, 10);
  EXPECT_EQ(restore->fec_transition->percentage_before, 15);
  EXPECT_EQ(restore->fec_transition->percentage_after, 10);
  EXPECT_EQ(restore->fec_transition->phase_before, "fixed");
  EXPECT_EQ(restore->fec_transition->phase_after, "raising_rate");
  EXPECT_FALSE(restore->fec_transition->pending_percentage_before);
  EXPECT_EQ(restore->fec_transition->pending_percentage_after, 10);
  EXPECT_FALSE(restore->fec_transition->from_frame_index);
  EXPECT_GT(restore->sequence, published->sequence);
  const auto before_restore_ack = adaptive_bitrate::get_state_with_history().history;
  EXPECT_TRUE(std::none_of(before_restore_ack.entries.begin(), before_restore_ack.entries.end(), [](const auto &entry) {
    return entry.kind == "fec_restored";
  }));
  EXPECT_EQ(restore->requested_bitrate_kbps, budget.encoder_kbps);
  EXPECT_TRUE(restore->applied_known);
  EXPECT_EQ(restore->applied_bitrate_kbps, trial_encoder);  // the higher rate is only requested
  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.current_fec_percentage, 10);
  EXPECT_EQ(state.fec_phase, "raising_rate");
  EXPECT_EQ(state.target_bitrate_kbps, budget.encoder_kbps);
  EXPECT_EQ(state.applied_bitrate_kbps, trial_encoder);
  // Before the higher rate's ACK, frames queued at the trial rate and every future frame use F0.
  const std::int64_t ack_frame = last_frame + 1;
  for (const std::int64_t frame : {publication_frame, last_frame, ack_frame, ack_frame + 1000}) {
    EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4934, frame, 77), (std::pair {10, 10})) << frame;
  }

  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_EQ(request->target_bitrate_kbps, budget.encoder_kbps);
  EXPECT_EQ(request->revision, restore->operator_revision);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*request,
    [] { return true; }, nullptr, ack_frame));
  state = adaptive_bitrate::get_state_with_history();
  const auto &entries = state.history.entries;
  const auto ack = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
    return entry.kind == "encoder_applied" && entry.callback_revision == request->revision;
  });
  ASSERT_NE(ack, entries.end());
  EXPECT_EQ(ack->cause, "runtime_update");
  EXPECT_GT(ack->sequence, restore->sequence);
  EXPECT_EQ(ack->callback_bitrate_kbps, budget.encoder_kbps);
  EXPECT_EQ(ack->applied_bitrate_kbps, budget.encoder_kbps);
  EXPECT_EQ(ack->applied_revision, request->revision);
  const auto restored = std::find_if(entries.begin(), entries.end(), [](const auto &entry) {
    return entry.kind == "fec_restored";
  });
  ASSERT_NE(restored, entries.end());
  ASSERT_TRUE(restored->fec_transition);
  EXPECT_EQ(restored->cause, "configured_parity_encoder_confirmed");
  EXPECT_EQ(restored->fec_transition->configured_percentage, 10);
  EXPECT_EQ(restored->fec_transition->percentage_before, 10);
  EXPECT_EQ(restored->fec_transition->percentage_after, 10);
  EXPECT_EQ(restored->fec_transition->phase_before, "raising_rate");
  EXPECT_EQ(restored->fec_transition->phase_after, "fixed");
  EXPECT_EQ(restored->fec_transition->pending_percentage_before, 10);
  EXPECT_FALSE(restored->fec_transition->pending_percentage_after);
  EXPECT_FALSE(restored->fec_transition->from_frame_index);
  EXPECT_EQ(restored->callback_bitrate_kbps, budget.encoder_kbps);
  EXPECT_EQ(restored->callback_revision, request->revision);
  EXPECT_EQ(restored->applied_revision, request->revision);
  EXPECT_GT(restored->sequence, ack->sequence);
  EXPECT_GT(restored->sequence, restore->sequence);
  EXPECT_EQ(std::count_if(entries.begin(), entries.end(), [](const auto &entry) {
    return entry.kind == "fec_restored";
  }), 1);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].fec_transition) EXPECT_EQ(entries[i].repeat_count, 1u);
    if (i) EXPECT_GT(entries[i].sequence, entries[i - 1].sequence);
  }
  EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [&](const auto &entry) {
    return entry.kind == "fec_retired" && entry.sequence > published->sequence;
  }));
  EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [](const auto &entry) {
    return entry.kind == "fec_refused";
  }));
  EXPECT_EQ(state.current_fec_percentage, 10);
  EXPECT_EQ(state.fec_phase, "fixed");
  EXPECT_EQ(state.applied_bitrate_kbps, budget.encoder_kbps);
  EXPECT_EQ(adaptive_bitrate::frame_fec_allocation(4934, ack_frame, 77), (std::pair {10, 10}));
  adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, budget.encoder_kbps, ack_frame + 1);
  const auto repeated_ack = adaptive_bitrate::get_state_with_history().history;
  EXPECT_EQ(repeated_ack.entries.size(), entries.size());
  EXPECT_EQ(std::count_if(repeated_ack.entries.begin(), repeated_ack.entries.end(), [](const auto &entry) {
    return entry.kind == "fec_restored";
  }), 1);
  adaptive_bitrate::reset();
  adaptive_bitrate::set_session_scope(0, {}, false);
}

TEST(AdaptiveBitrateController, OneLossyReportNeverCuts) {
  // A single burst, such as one second of Wi-Fi interference, is not a change in the stream's loss.
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  sustain_media_loss(40.0, 1);
  sustain_media_loss(0.0, 5);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_NE(state.state, "network_pressure");
}

TEST(AdaptiveBitrateController, AHighRttReadingAloneNeverCuts) {
  // 1.4.13 cut 10% for one reading of 45 ms and over twice the average. A Steam Deck's Wi-Fi background
  // scan lifts ENet's reading to 83 ms for 5 s every 306.9 s, so that cut every five minutes on a clean
  // link. The policy cuts for a queue only after the reading has been raised and not falling for 8 s.
  enable_controller();

  for (int i = 0; i < 6; ++i) {
    adaptive_bitrate::update_rtt(3.0);
  }
  for (int i = 0; i < 22; ++i) {
    std::this_thread::sleep_for(100ms);
    adaptive_bitrate::update_rtt(83.0);
  }

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_NE(state.state, "network_pressure");
}

TEST(AdaptiveBitrateController, MediaReportsWithoutFramesOrWithMoreLostThanExpectedAreNoEvidence) {
  enable_controller();
  adaptive_bitrate::update_rtt(8.0);
  for (int i = 0; i < 5; ++i) {
    adaptive_bitrate::update_media_report(0.0, 0.0, 8.0);
    adaptive_bitrate::update_media_report(120.0, 121.0, 8.0);
    adaptive_bitrate::update_media_report(std::nan(""), 10.0, 8.0);
  }
  EXPECT_EQ(adaptive_bitrate::get_state().target_bitrate_kbps, 20000);
  EXPECT_EQ(adaptive_bitrate::get_state().ewma_packet_loss, 0.0);
}

TEST(AdaptiveBitrateController, TelemetryMovementInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  sustain_media_loss(8.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  ASSERT_LT(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  ASSERT_GT(after_pressure.revision, before_pressure.revision);
  EXPECT_GT(
    after_pressure.action_authority_revision,
    before_pressure.action_authority_revision
  );
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_pressure.revision,
    before_pressure.live_bitrate_kbps,
    before_pressure.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, NetworkPressureAtFloorInvalidatesAStaleDoctorSnapshot) {
  enable_controller(2000);

  adaptive_bitrate::update_rtt(8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  // A probe at the floor cannot move the target, and still starts a new decision.
  sustain_media_loss(8.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  ASSERT_EQ(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  ASSERT_GT(after_pressure.revision, before_pressure.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_pressure.revision,
    3000,
    before_pressure.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, HostEvidenceInsideAdjustmentIntervalInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  const auto before_observation = adaptive_bitrate::get_doctor_state();
  // Production publishes the host evidence epoch before feeding the adaptive
  // loop. No interval sleep is intentional: this covers the early-return path.
  adaptive_bitrate::note_network_evidence_arrival(true);
  adaptive_bitrate::update_rtt(55.0);

  const auto after_observation = adaptive_bitrate::get_doctor_state();
  ASSERT_EQ(after_observation.live_bitrate_kbps, before_observation.live_bitrate_kbps);
  ASSERT_GT(after_observation.revision, before_observation.revision);
  EXPECT_EQ(
    after_observation.action_authority_revision,
    before_observation.action_authority_revision
  );
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_observation.revision,
    25000,
    before_observation.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, VerificationEvidenceDoesNotSupersedeAnOwnedDoctorTarget) {
  enable_controller();
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::note_network_evidence_arrival(false);

  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, *doctor_revision);
  EXPECT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ).has_value());
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, VideoRegressionRemainsLatchedForADoctorTransaction) {
  enable_controller();
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  EXPECT_TRUE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, *doctor_revision);

  const auto blocked_step =
    adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
      *doctor_revision,
      18000,
      before.max_bitrate_kbps
    );
  EXPECT_EQ(
    blocked_step.status,
    adaptive_bitrate::doctor_bitrate_apply_status_e::quality_policy_blocked
  );
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, *doctor_revision);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);

  EXPECT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, NetworkRegressionBlocksTheNextDoctorQualityStep) {
  enable_controller();
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision =
    adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
      before.revision,
      16000,
      before.max_bitrate_kbps
    );
  ASSERT_EQ(
    doctor_revision.status,
    adaptive_bitrate::doctor_bitrate_apply_status_e::applied
  );

  adaptive_bitrate::note_network_evidence_arrival(true);
  adaptive_bitrate::note_network_evidence_arrival(false);
  EXPECT_TRUE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    doctor_revision.revision
  );

  const auto blocked_step =
    adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
      doctor_revision.revision,
      18000,
      before.max_bitrate_kbps
    );
  EXPECT_EQ(
    blocked_step.status,
    adaptive_bitrate::doctor_bitrate_apply_status_e::quality_policy_blocked
  );
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);

  EXPECT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    doctor_revision.revision,
    before
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, ReportsFramePacingWithoutLoweringBitrate) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.75, 0.05, 0.02, 3.0, 4.0, 28.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_EQ(state.base_bitrate_kbps, state.target_bitrate_kbps);
  EXPECT_EQ("frame_pacing_observed", state.state);
}

TEST(AdaptiveBitrateController, ReducesTargetOnEncoderPressure) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ("encoder_pressure", state.state);
}

TEST(AdaptiveBitrateController, HighRefreshEncoderPressureRequiresAnOverrunAndDeliveryShortfall) {
  struct sample_t { double fps, ratio, encode_ms; bool reduce; };
  const double unknown = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const sample_t samples[] = {
    {60.0, 0.80, 9.0, false},       // Existing low-refresh behavior.
    {60.0, 1.00, 11.0, true},
    {120.0, 0.90, 9.0, true},
    {165.0, 0.90, 6.1, true},
    {240.0, 0.80, 4.2, true},       // Old fixed threshold missed this overrun.
    {240.0, 1.00, 6.0, false},      // Isolated slow encode, healthy delivery.
    {240.0, 0.95, 6.0, false},
    {240.0, 0.50, 3.0, false},      // Pacing/capture shortage, encoder within budget.
    {240.0, 0.00, 6.0, false},      // No measured delivery yet.
    {240.0, unknown, 6.0, false},
    {0.0, 0.80, 6.0, false},
    {-240.0, 0.80, 6.0, false},
    {unknown, 0.80, 6.0, false},
    {infinity, 0.80, 6.0, false},
    {240.0, 0.80, unknown, false},
    {240.0, 0.80, infinity, false},
    {120000.0 / 1001.0, 0.80, 8.335, false}, // Do not round fractional targets.
    {120000.0 / 1001.0, 0.80, 8.345, true},
  };
  for (const auto &sample : samples) {
    SCOPED_TRACE(::testing::Message() << sample.fps << " fps, ratio=" << sample.ratio << ", work=" << sample.encode_ms);
    enable_controller();
    adaptive_bitrate::update_rtt(8.0);
    std::this_thread::sleep_for(1100ms);
    const auto before = adaptive_bitrate::get_doctor_state();
    adaptive_bitrate::update_stream_health(sample.ratio, 0.0, 0.0, 0.0, sample.encode_ms, 0.0, sample.fps);
    const auto state = adaptive_bitrate::get_state();
    if (sample.reduce) {
      EXPECT_EQ(state.target_bitrate_kbps, 17600);
      EXPECT_EQ(state.state, "encoder_pressure");
      EXPECT_EQ(state.reason, "encode_load");
      EXPECT_GT(adaptive_bitrate::get_doctor_state().revision, before.revision);
    } else {
      EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);
      EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, before.revision);
    }
  }
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, EncoderPressureMovementAdvancesControllerRevision) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_LT(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
}

TEST(AdaptiveBitrateController, EncoderPressureInsideAdjustmentIntervalInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_rtt(8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  // No interval sleep is intentional: a pressure sample must invalidate a
  // stale Doctor snapshot even when the adaptive target cannot move yet.
  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_pressure.revision,
    25000,
    before_pressure.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, VideoPolicyTransitionsInvalidateOnceWithAdaptiveDisabled) {
  enable_controller();
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  const auto clean_observation = adaptive_bitrate::get_doctor_state();

  // Repeated clean samples retain a still-valid action envelope.
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    clean_observation.revision
  );

  // The first host video warning suppresses restore_quality even though the
  // adaptive controller itself is disabled and would ignore this sample.
  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  const auto warning_observation = adaptive_bitrate::get_doctor_state();
  EXPECT_FALSE(warning_observation.enabled);
  EXPECT_GT(warning_observation.revision, clean_observation.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    clean_observation.revision,
    25000,
    clean_observation.max_bitrate_kbps
  ));

  // Persistent warning samples do not starve an unrelated stable network
  // action by continuously rotating controller authority.
  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    warning_observation.revision
  );
}

TEST(AdaptiveBitrateController, ProvisionalPacingTransitionInvalidatesWithoutBlockingRestore) {
  enable_controller();
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::note_doctor_video_policy_evidence(false, false);
  const auto clean_observation = adaptive_bitrate::get_doctor_state();

  adaptive_bitrate::note_doctor_video_policy_evidence(false, true);
  const auto provisional_observation = adaptive_bitrate::get_doctor_state();
  EXPECT_GT(provisional_observation.revision, clean_observation.revision);
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    clean_observation.revision,
    25000,
    clean_observation.max_bitrate_kbps
  ));

  adaptive_bitrate::note_doctor_video_policy_evidence(false, true);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    provisional_observation.revision
  );
}

TEST(AdaptiveBitrateController, EncoderPressureAtFloorAdvancesControllerRevision) {
  enable_controller(2000);

  adaptive_bitrate::update_rtt(8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
}

TEST(AdaptiveBitrateController, ConfiguredCeilingNeverCutsTheClientsStartingBitrate) {
  // A PyroWave client asking for 180 Mbps at 1080p60 reaches the encoder at
  // about 161 Mbps, far above the 50 Mbps ceiling configured here.
  enable_controller(161000);

  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(161000, state.base_bitrate_kbps);
  EXPECT_EQ(161000, state.target_bitrate_kbps);
  // The ceiling the controller holds, and the host reports, is the request.
  EXPECT_EQ(161000, state.max_bitrate_kbps);
  // Adaptive is on, so the encoder polls this request every 30 frames. It has
  // to hold the rate the stream opened at rather than cut it to the ceiling.
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(161000, request->target_bitrate_kbps);

  // Pressure lowers the bitrate from the client's request, not from the ceiling.
  adaptive_bitrate::update_rtt(8.0);
  sustain_media_loss(8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ("network_pressure", state.state);
  EXPECT_EQ(state.target_bitrate_kbps, 96600);
  EXPECT_GT(state.target_bitrate_kbps, 50000);

  // The raise belongs to the stream. It never reaches the configured value, so
  // the next stream start reloads 50 Mbps for a smaller stream after this one.
  EXPECT_EQ(50000, config::video.adaptive_bitrate.max_bitrate_kbps);
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_base_bitrate(20000);
  EXPECT_EQ(50000, adaptive_bitrate::get_state().max_bitrate_kbps);
}

TEST(AdaptiveBitrateController, ARequestBelowTheConfiguredCeilingStartsAsBefore) {
  // A guard, not proof of the fix: this passed before it too. It pins that a
  // stream under the configured ceiling starts exactly as it did, and fails if
  // the fix sets the ceiling to the request instead of raising it to the request.
  enable_controller(20000);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(20000, state.base_bitrate_kbps);
  EXPECT_EQ(20000, state.target_bitrate_kbps);
  EXPECT_EQ(50000, state.max_bitrate_kbps);
}

TEST(AdaptiveBitrateController, ARequestBelowTheFloorStartsAtTheFloor) {
  // A guard, not proof of the fix: this held before it too. The floor is the one
  // place the bitrate can sit above what a client asked for, and the Live Tuning
  // docs say so. A client asking 10 Mbps opens the encoder near 7988 kbps; with
  // the floor at 20000 the controller starts at 20000, and the rewritten live
  // write has to keep that floor as well.
  struct restore_adaptive_config_t {
    decltype(config::video.adaptive_bitrate) saved = config::video.adaptive_bitrate;

    ~restore_adaptive_config_t() {
      config::video.adaptive_bitrate = saved;
    }
  } restore;
  config::video.adaptive_bitrate.enabled = true;
  config::video.adaptive_bitrate.min_bitrate_kbps = 20000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 7988);
  adaptive_bitrate::set_base_bitrate(7988);

  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(20000, state.base_bitrate_kbps);
  EXPECT_EQ(50000, state.max_bitrate_kbps);
  // With Live Tuning on, the encoder's first bitrate check raises it to the floor.
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(20000, request->target_bitrate_kbps);

  adaptive_bitrate::set_live_bitrate(10000);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(20000, state.base_bitrate_kbps);
  EXPECT_EQ(20000, state.target_bitrate_kbps);
}

TEST(AdaptiveBitrateController, ExplicitLiveRetryCanRaiseSessionCeilingAndTarget) {
  enable_controller(7580);
  const auto configured_ceiling = config::video.adaptive_bitrate.max_bitrate_kbps;
  adaptive_bitrate::set_max_bitrate(7580);
  adaptive_bitrate::set_max_bitrate(20000);
  adaptive_bitrate::set_live_bitrate(9475);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.max_bitrate_kbps, 20000);
  EXPECT_EQ(state.base_bitrate_kbps, 9475);
  EXPECT_EQ(state.target_bitrate_kbps, 9475);
  EXPECT_EQ(state.reason, "paired_client_action");
  EXPECT_EQ(config::video.adaptive_bitrate.max_bitrate_kbps, configured_ceiling);
}

TEST(AdaptiveBitrateController, ExplicitLiveWriteAboveTheConfiguredCeilingIsKept) {
  enable_controller(20000);
  adaptive_bitrate::set_live_bitrate(180000);

  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(180000, state.base_bitrate_kbps);
  EXPECT_EQ(180000, state.target_bitrate_kbps);
  EXPECT_EQ(180000, state.max_bitrate_kbps);
  EXPECT_EQ(50000, config::video.adaptive_bitrate.max_bitrate_kbps);

  // Adaptive stays on in this controller, so pressure works down from the
  // written 180 Mbps and must not snap to the configured 50 Mbps ceiling.
  adaptive_bitrate::update_rtt(8.0);
  sustain_media_loss(8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.target_bitrate_kbps, 108000);
  EXPECT_GT(state.target_bitrate_kbps, 50000);
}

TEST(AdaptiveBitrateController, ExplicitLiveWriteStaysUnderTheHostMaxBitrate) {
  const auto host_cap = config::video.max_bitrate;
  enable_controller(20000);
  config::video.max_bitrate = 150000;
  adaptive_bitrate::set_live_bitrate(180000);
  const auto capped = adaptive_bitrate::get_state();
  config::video.max_bitrate = 0;
  adaptive_bitrate::set_live_bitrate(180000);
  const auto uncapped = adaptive_bitrate::get_state();
  config::video.max_bitrate = host_cap;

  EXPECT_EQ(150000, capped.base_bitrate_kbps);
  EXPECT_EQ(150000, capped.target_bitrate_kbps);
  EXPECT_EQ(180000, uncapped.base_bitrate_kbps);
  EXPECT_EQ(180000, uncapped.target_bitrate_kbps);
}

TEST(AdaptiveBitrateController, HidesTargetsWhenEncoderCannotApplyRuntimeUpdates) {
  enable_controller(26000);
  adaptive_bitrate::set_runtime_update_supported(false);

  sustain_media_loss(21.8, 5, 12.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_FALSE(state.active);
  EXPECT_FALSE(state.runtime_update_supported);
  EXPECT_EQ(0, state.target_bitrate_kbps);
  EXPECT_EQ(0, adaptive_bitrate::get_target_bitrate_kbps());
  EXPECT_EQ("unavailable", state.state);
  EXPECT_EQ("encoder_runtime_update_unsupported", state.reason);
}

TEST(AdaptiveBitrateController, NormalizesMaxBelowMinWithoutCuttingTheBase) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 0;

  adaptive_bitrate::load_config();
  EXPECT_EQ(2000, config::video.adaptive_bitrate.max_bitrate_kbps);
  adaptive_bitrate::reset();
  // The controller's own ceiling is normalized up to the floor as well.
  EXPECT_EQ(2000, adaptive_bitrate::get_state().max_bitrate_kbps);
  adaptive_bitrate::set_base_bitrate(30000);

  const auto state = adaptive_bitrate::get_state();
  // The stream then raises that ceiling to its request rather than being cut to it.
  EXPECT_EQ(30000, state.max_bitrate_kbps);
  EXPECT_EQ(30000, state.base_bitrate_kbps);
  EXPECT_EQ(0, state.target_bitrate_kbps);
}

TEST(AdaptiveBitrateController, DoctorRollbackNeverOverwritesANewerExplicitWriter) {
  enable_controller(20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    20000
  );
  ASSERT_TRUE(doctor_revision.has_value());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 15000);

  adaptive_bitrate::set_base_bitrate(10000);
  EXPECT_FALSE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ));

  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.base_bitrate_kbps, 10000);
  EXPECT_EQ(after.live_bitrate_kbps, 10000);
  EXPECT_GT(after.revision, *doctor_revision);
}

TEST(AdaptiveBitrateController, DoctorTransactionRestoresExactOwnedState) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  ASSERT_FALSE(before.enabled);

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    25000
  );
  ASSERT_TRUE(doctor_revision.has_value());
  // Doctor owns one live target without changing the configured/runtime
  // adaptive-controller mode.
  ASSERT_FALSE(adaptive_bitrate::get_doctor_state().enabled);
  ASSERT_TRUE(adaptive_bitrate::is_active());
  ASSERT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 15000);
  adaptive_bitrate::acknowledge_live_bitrate_applied(*doctor_revision, 15000);

  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());
  const auto rollback_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(rollback_request.has_value());
  EXPECT_EQ(rollback_request->revision, *restore_revision);
  EXPECT_EQ(rollback_request->target_bitrate_kbps, before.live_bitrate_kbps);
  EXPECT_TRUE(adaptive_bitrate::get_state().active);
  EXPECT_EQ(adaptive_bitrate::get_state().state, "rollback_pending");

  adaptive_bitrate::acknowledge_live_bitrate_applied(
    rollback_request->revision,
    rollback_request->target_bitrate_kbps
  );
  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.enabled, before.enabled);
  EXPECT_EQ(after.base_bitrate_kbps, before.base_bitrate_kbps);
  EXPECT_EQ(after.live_bitrate_kbps, before.live_bitrate_kbps);
  EXPECT_EQ(after.max_bitrate_kbps, before.max_bitrate_kbps);
  EXPECT_FALSE(adaptive_bitrate::is_active());
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    before.live_bitrate_kbps
  ).has_value());
}

TEST(AdaptiveBitrateController, RecreatedEncoderSessionAcknowledgesTheExactPendingRevision) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *doctor_revision,
    15000
  ).has_value());

  // The FFmpeg path recreates only its encoder session. A successful open at
  // the requested config reports that exact target and current controller
  // revision through this existing session-ready handshake.
  adaptive_bitrate::set_runtime_update_supported(true, {}, 15000);
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    *doctor_revision,
    15000
  ).has_value());
}

TEST(AdaptiveBitrateController, EncoderSessionLossInvalidatesItsApplicationProof) {
  enable_controller(20000);
  const auto state = adaptive_bitrate::get_doctor_state();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  ASSERT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    state.revision,
    20000
  ).has_value());

  adaptive_bitrate::set_runtime_update_supported(false, "encoder_session_ended");
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    state.revision,
    20000
  ).has_value());
}

TEST(AdaptiveBitrateController, RollbackDuringEncoderRecreationWaitsForTheReplacementSession) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(
    *doctor_revision,
    15000
  ));

  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::wait_for_live_bitrate_applied(
    *restore_revision,
    20000,
    1ms
  ));

  const auto rollback_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(rollback_request.has_value());
  EXPECT_EQ(rollback_request->revision, *restore_revision);
  EXPECT_EQ(rollback_request->target_bitrate_kbps, 20000);

  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  EXPECT_TRUE(adaptive_bitrate::wait_for_live_bitrate_applied(
    *restore_revision,
    20000,
    1ms
  ));
}

TEST(AdaptiveBitrateController, SupersededRequestCannotRetireTheCurrentEncoderSession) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());

  EXPECT_FALSE(adaptive_bitrate::begin_live_bitrate_session_recreation(
    *doctor_revision,
    15000
  ));
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());
}

TEST(AdaptiveBitrateController, RetryBeforeReplacementCannotSynthesizeEncoderProof) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto first_doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(first_doctor_revision.has_value());
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(
    *first_doctor_revision,
    15000
  ));

  const auto first_restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *first_doctor_revision,
    before
  );
  ASSERT_TRUE(first_restore_revision.has_value());
  ASSERT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *first_restore_revision,
    20000
  ).has_value());

  const auto retry_before = adaptive_bitrate::get_doctor_state();
  const auto retry_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    retry_before.revision,
    16000,
    retry_before.max_bitrate_kbps
  );
  ASSERT_TRUE(retry_revision.has_value());
  const auto retry_restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *retry_revision,
    retry_before
  );
  ASSERT_TRUE(retry_restore_revision.has_value());

  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *retry_restore_revision,
    20000
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::wait_for_live_bitrate_applied(
    *retry_restore_revision,
    20000,
    1ms
  ));
}

TEST(AdaptiveBitrateController, DoctorTargetCannotDriftFromTelemetry) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    20000
  );
  ASSERT_TRUE(doctor_revision.has_value());

  sustain_media_loss(12.0, 5, 120.0);
  adaptive_bitrate::update_rtt(120.0);
  adaptive_bitrate::update_stream_health(0.70, 0.10, 0.20, 8.0, 18.0, 50.0);

  const auto during = adaptive_bitrate::get_doctor_state();
  EXPECT_FALSE(during.enabled);
  EXPECT_EQ(during.base_bitrate_kbps, 16000);
  EXPECT_EQ(during.live_bitrate_kbps, 16000);
  EXPECT_EQ(during.revision, *doctor_revision);

  ASSERT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ));
}

TEST(AdaptiveBitrateController, RollbackTargetCannotDriftBeforeEncoderAcknowledgement) {
  enable_controller(20000);
  adaptive_bitrate::update_rtt(8.0);
  const auto before = adaptive_bitrate::get_doctor_state();
  ASSERT_TRUE(before.enabled);

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  adaptive_bitrate::acknowledge_live_bitrate_applied(*doctor_revision, 16000);

  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());
  ASSERT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 20000);

  std::this_thread::sleep_for(1100ms);
  sustain_media_loss(12.0, 5, 120.0);
  adaptive_bitrate::update_stream_health(0.70, 0.10, 0.20, 8.0, 18.0, 50.0);

  const auto pending = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(pending.live_bitrate_kbps, 20000);
  EXPECT_EQ(pending.revision, *restore_revision);
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());

  adaptive_bitrate::acknowledge_live_bitrate_applied(*restore_revision, 20000);
  sustain_media_loss(12.0, 3, 120.0);

  const auto resumed = adaptive_bitrate::get_doctor_state();
  EXPECT_LT(resumed.live_bitrate_kbps, 20000);
  EXPECT_GT(resumed.revision, *restore_revision);
}

TEST(AdaptiveBitrateController, NewerExplicitIncreaseReplacesDoctorTargetExactly) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    20000
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::set_live_bitrate(30000);

  EXPECT_FALSE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ));
  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.base_bitrate_kbps, 30000);
  EXPECT_EQ(after.live_bitrate_kbps, 30000);
  EXPECT_EQ(after.max_bitrate_kbps, before.max_bitrate_kbps);
  EXPECT_TRUE(after.explicit_live_override_active);
  EXPECT_TRUE(adaptive_bitrate::is_active());
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 30000);
  EXPECT_GT(after.revision, *doctor_revision);
}

TEST(AdaptiveBitrateController, DisableRejectsFetchedButUnappliedRequest) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  adaptive_bitrate::set_enabled(false);
  bool called = false;
  EXPECT_FALSE(adaptive_bitrate::apply_live_bitrate_request(*request, [&] { called = true; return true; }));
  EXPECT_FALSE(called);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, DisableHoldsCompletedEncoderApplication) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*request, [] { return true; }));
  adaptive_bitrate::set_enabled(false);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 14000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, DisableDuringRecreationReconcilesToLastProvenRate) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(request->revision, 14000));
  adaptive_bitrate::set_enabled(false);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 0);
  ASSERT_TRUE(adaptive_bitrate::get_live_bitrate_request());
  EXPECT_EQ(adaptive_bitrate::get_live_bitrate_request()->target_bitrate_kbps, 20000);
  // If creation already began with the old target, its actual rate remains
  // visible until the held target is reconciled, even with tuning disabled.
  adaptive_bitrate::set_runtime_update_supported(true, {}, 14000);
  const auto reconcile = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(reconcile);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*reconcile, [] { return true; }));
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, DisableSerializesWithInFlightEncoderApplication) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = *adaptive_bitrate::get_live_bitrate_request();
  std::promise<void> entered, finish;
  auto resume = finish.get_future();
  auto update = std::async(std::launch::async, [&] {
    return adaptive_bitrate::apply_live_bitrate_request(request, [&] {
      entered.set_value(); resume.wait(); return true;
    });
  });
  entered.get_future().wait();
  auto disable = std::async(std::launch::async, [] { adaptive_bitrate::set_enabled(false); });
  EXPECT_EQ(disable.wait_for(20ms), std::future_status::timeout);
  finish.set_value();
  EXPECT_TRUE(update.get());
  disable.get();
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 14000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, PyroWaveFloorStopsLiveTuningAndNeverLiftsARequest) {
  // A request below the codec's floor becomes the floor itself: Live Tuning cannot cut it at all, and
  // the stream is never raised above what the client asked for.
  enable_controller(20000);
  adaptive_bitrate::set_session_floor(76785, "pyrowave_advice");
  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.min_bitrate_kbps, 20000);
  EXPECT_EQ(state.base_bitrate_kbps, 20000);
  EXPECT_EQ(state.target_bitrate_kbps, 20000);
  EXPECT_EQ(state.floor_source, "pyrowave_advice");

  // Above the floor, a probe goes no lower than it. Heavy loss the floor does not help is not the
  // bitrate's doing, so the stream goes back to its request rather than softening the picture for nothing.
  enable_controller(100000);
  adaptive_bitrate::set_session_floor(90000, "pyrowave_advice");
  ASSERT_EQ(adaptive_bitrate::get_state().min_bitrate_kbps, 90000);
  adaptive_bitrate::update_rtt(8.0);
  int lowest = 100000;
  for (int i = 0; i < 12; ++i) {
    sustain_media_loss(20.0, 1);
    lowest = std::min(lowest, adaptive_bitrate::get_state().target_bitrate_kbps);
  }
  EXPECT_EQ(lowest, 90000);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.target_bitrate_kbps, 100000);
  EXPECT_EQ(state.reason, "loss_independent_of_bitrate");

  // Loss the floor does help, over a link of 95 Mbps, holds between the floor and the link.
  enable_controller(100000);
  adaptive_bitrate::set_session_floor(90000, "pyrowave_advice");
  adaptive_bitrate::update_rtt(8.0);
  std::vector<int> targets;
  for (int i = 0; i < 80; ++i) {
    sustain_media_loss(adaptive_bitrate::get_state().target_bitrate_kbps > 95000 ? 20.0 : 0.0, 1);
    targets.push_back(adaptive_bitrate::get_state().target_bitrate_kbps);
  }
  EXPECT_EQ(*std::min_element(targets.begin(), targets.end()), 90000);
  // Settled under the link, trying the request again only now and then.
  const auto over_link = std::count_if(targets.end() - 40, targets.end(), [](int t) {
    return t > 95000;
  });
  EXPECT_LE(over_link, 4);

  // The next stream starts from adaptive_bitrate_min again.
  enable_controller(20000);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.min_bitrate_kbps, 2000);
  EXPECT_EQ(state.floor_source, "adaptive_bitrate_min");
}

namespace {
  /// One client media report: the frames the client expected in its second, and how many never arrived.
  struct media_report_t {
    double expected;
    double lost;
  };

  // The Retroid Pocket 6's recorded HEVC run, 3840x2160 at 120 fps over Wi-Fi: 9 of 122 frames lost and 9
  // of 121, in two of every eight reports, for five minutes, and the host's RTT through it. The same run
  // test_stream_stats.cpp replays through Doctor's judge.
  constexpr std::array<media_report_t, 8> k_rp6_hevc_reports {{
    {120, 0}, {122, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {121, 9}, {120, 0}
  }};
  constexpr std::array<double, 8> k_rp6_hevc_rtt_ms {4.0, 4.4, 10.1, 7.6, 9.6, 7.9, 8.6, 4.3};
  constexpr int k_rp6_hevc_kbps = 268988;

  /// Leave no cut target behind for a later suite in this binary to read as a reduced stream.
  void leave_live_tuning_clean() {
    adaptive_bitrate::reset();
    adaptive_bitrate::set_enabled(false);
    config::video.adaptive_bitrate.enabled = false;
  }
}  // namespace

TEST(AdaptiveBitrateController, TheRecordedRp6HevcRunNeverProbesAndKeepsItsBitrate) {
  // Loss in two of every eight reports, whatever the rate, and never in two of three: no probe starts, and
  // the stream keeps the request for the whole run.
  enable_controller(k_rp6_hevc_kbps);
  config::video.adaptive_bitrate.max_bitrate_kbps = 300000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_base_bitrate(k_rp6_hevc_kbps);
  int moved = 0;
  for (int report = 0; report < 300; ++report) {
    const auto &recorded = k_rp6_hevc_reports[report % k_rp6_hevc_reports.size()];
    adaptive_bitrate::update_rtt(k_rp6_hevc_rtt_ms[report % k_rp6_hevc_rtt_ms.size()]);
    adaptive_bitrate::update_media_report(recorded.expected, recorded.lost, k_rp6_hevc_rtt_ms[report % k_rp6_hevc_rtt_ms.size()]);
    moved += adaptive_bitrate::get_state().target_bitrate_kbps != k_rp6_hevc_kbps;
  }
  EXPECT_EQ(moved, 0);
  EXPECT_EQ(adaptive_bitrate::get_state().state, "steady");
  leave_live_tuning_clean();
}

TEST(AdaptiveBitrateController, ATwentyPercentBurstIsBackAtTheRequestSoonAfterItEnds) {
  // Three seconds that lose a fifth of their frames at every rate, then clean reports.
  enable_controller(k_rp6_hevc_kbps);
  config::video.adaptive_bitrate.max_bitrate_kbps = 300000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_base_bitrate(k_rp6_hevc_kbps);
  adaptive_bitrate::update_rtt(8.0);
  sustain_media_loss(0.0, 20);
  sustain_media_loss(20.0, 3);
  EXPECT_LT(adaptive_bitrate::get_state().target_bitrate_kbps, k_rp6_hevc_kbps);
  int back_after = -1;
  for (int report = 1; report <= 40 && back_after < 0; ++report) {
    sustain_media_loss(0.0, 1);
    if (adaptive_bitrate::get_state().target_bitrate_kbps == k_rp6_hevc_kbps) {
      back_after = report;
    }
  }
  EXPECT_GT(back_after, 0);
  EXPECT_LE(back_after, 30);
  leave_live_tuning_clean();
}

namespace {
  constexpr std::uint64_t k_history_generation = 41;

  // A stream scope as stream start leaves it: load_config(), reset(), base bitrate, then the scope.
  void start_history_scope(int base_bitrate_kbps = 20000) {
    enable_controller(base_bitrate_kbps);
    adaptive_bitrate::set_session_scope(k_history_generation, "history-owner-a", true);
  }

  adaptive_bitrate::history_t history_now() {
    return adaptive_bitrate::get_state_with_history().history;
  }

  std::vector<adaptive_bitrate::history_entry_t> entries_of(const adaptive_bitrate::history_t &history,
                                                            std::string_view kind) {
    std::vector<adaptive_bitrate::history_entry_t> out;
    for (const auto &entry : history.entries) {
      if (entry.kind == kind) out.push_back(entry);
    }
    return out;
  }

  // Ask for the current encoder request and apply it the way the encoder thread does.
  adaptive_bitrate::live_bitrate_request_t apply_current_request(bool backend_applies) {
    const auto request = adaptive_bitrate::get_live_bitrate_request();
    EXPECT_TRUE(request.has_value());
    if (!request) return {};
    EXPECT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*request, [&] { return backend_applies; }));
    return *request;
  }

  void leave_history_clean() {
    adaptive_bitrate::set_session_scope(0, {}, false);
    leave_live_tuning_clean();
  }
}  // namespace

TEST(AdaptiveBitrateHistory, KeepsATransitionAndItsReversalThatOneReadWouldMiss) {
  start_history_scope();
  apply_current_request(true);
  const auto before = adaptive_bitrate::get_state();
  const auto first = history_now();

  // Turned off and back on, and a target moved and put back, all between two reads.
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_enabled(true);
  adaptive_bitrate::set_live_bitrate(15000);
  adaptive_bitrate::set_live_bitrate(20000);

  const auto after = adaptive_bitrate::get_state();
  EXPECT_EQ(after.target_bitrate_kbps, before.target_bitrate_kbps);
  const auto history = history_now();
  const auto decisions = entries_of(history, "decision");
  ASSERT_GE(decisions.size(), 4u);
  const auto tail = std::vector<adaptive_bitrate::history_entry_t>(decisions.end() - 4, decisions.end());
  EXPECT_EQ(tail[0].state, "disabled");
  EXPECT_EQ(tail[0].cause, "runtime_enabled");
  EXPECT_EQ(tail[1].state, before.state);
  EXPECT_EQ(tail[1].cause, "runtime_enabled");
  EXPECT_EQ(tail[2].requested_bitrate_kbps, 15000);
  EXPECT_EQ(tail[2].reason, "paired_client_action");
  EXPECT_EQ(tail[3].requested_bitrate_kbps, 20000);
  for (std::size_t i = 1; i < tail.size(); ++i) {
    EXPECT_GT(tail[i].sequence, tail[i - 1].sequence);
    EXPECT_GE(tail[i].host_elapsed_ms, tail[i - 1].host_elapsed_ms);
  }
  EXPECT_GT(history.entries.size(), first.entries.size());
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, SeparatesAnAppliedUpdateFromARefusedOrStaleEncoderCallback) {
  start_history_scope();
  const auto applied = apply_current_request(true);
  adaptive_bitrate::set_live_bitrate(15000);
  const auto refused = apply_current_request(false);

  bool backend_called = false;
  EXPECT_FALSE(adaptive_bitrate::apply_live_bitrate_request(applied, [&] {
    backend_called = true;
    return true;
  }));
  EXPECT_FALSE(backend_called);

  const auto history = history_now();
  const auto applied_entries = entries_of(history, "encoder_applied");
  ASSERT_EQ(applied_entries.size(), 1u);
  EXPECT_EQ(applied_entries[0].callback_bitrate_kbps, 20000);
  EXPECT_EQ(applied_entries[0].callback_revision, applied.revision);
  EXPECT_TRUE(applied_entries[0].applied_known);
  EXPECT_EQ(applied_entries[0].applied_bitrate_kbps, 20000);

  const auto refused_entries = entries_of(history, "encoder_update_not_applied");
  ASSERT_EQ(refused_entries.size(), 1u);
  EXPECT_EQ(refused_entries[0].callback_bitrate_kbps, 15000);
  EXPECT_EQ(refused_entries[0].callback_revision, refused.revision);
  EXPECT_EQ(refused_entries[0].requested_bitrate_kbps, 15000);
  // The refused request is not an applied rate; the encoder still runs at the earlier one.
  EXPECT_EQ(refused_entries[0].applied_bitrate_kbps, 20000);

  const auto stale_entries = entries_of(history, "stale_encoder_request");
  ASSERT_EQ(stale_entries.size(), 1u);
  EXPECT_EQ(stale_entries[0].callback_revision, applied.revision);
  EXPECT_LT(applied_entries[0].sequence, refused_entries[0].sequence);
  EXPECT_LT(refused_entries[0].sequence, stale_entries[0].sequence);
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, RecreationLeavesTheAppliedRateUnknownUntilTheEncoderAcknowledges) {
  start_history_scope();
  const auto first = apply_current_request(true);
  adaptive_bitrate::set_live_bitrate(15000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());

  EXPECT_FALSE(adaptive_bitrate::begin_live_bitrate_session_recreation(first.revision, first.target_bitrate_kbps));
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(request->revision, request->target_bitrate_kbps));
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 0);
  adaptive_bitrate::acknowledge_live_bitrate_applied(first.revision, first.target_bitrate_kbps);
  adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, request->target_bitrate_kbps);
  const auto settled = history_now().entries.size();
  // The encoder confirms an unchanged rate every frame; that proves nothing new.
  adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, request->target_bitrate_kbps);

  const auto history = history_now();
  EXPECT_EQ(history.entries.size(), settled);
  ASSERT_EQ(entries_of(history, "recreation_superseded").size(), 1u);
  const auto invalidated = entries_of(history, "applied_proof_invalidated");
  ASSERT_EQ(invalidated.size(), 1u);
  EXPECT_FALSE(invalidated[0].applied_known);
  EXPECT_EQ(invalidated[0].applied_bitrate_kbps, 0);
  EXPECT_EQ(invalidated[0].requested_bitrate_kbps, 15000);
  ASSERT_EQ(entries_of(history, "stale_acknowledgement").size(), 1u);
  const auto acknowledged = entries_of(history, "encoder_acknowledged");
  ASSERT_EQ(acknowledged.size(), 1u);
  EXPECT_EQ(acknowledged[0].cause, "applied_proof_was_unknown");
  EXPECT_TRUE(acknowledged[0].applied_known);
  EXPECT_EQ(acknowledged[0].applied_bitrate_kbps, 15000);
  EXPECT_EQ(acknowledged[0].applied_revision, request->revision);
  EXPECT_GT(acknowledged[0].sequence, invalidated[0].sequence);
  // Recreation is not a backend update of the running encoder.
  EXPECT_EQ(entries_of(history, "encoder_applied").size(), 1u);
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, LabelsARevisionOnlyReaffirmationApartFromABackendUpdate) {
  start_history_scope();
  apply_current_request(true);
  adaptive_bitrate::note_network_evidence_arrival(false);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());

  auto history = history_now();
  const auto reaffirmed = entries_of(history, "revision_reaffirmed");
  ASSERT_EQ(reaffirmed.size(), 1u);
  EXPECT_EQ(reaffirmed[0].cause, "encoder_poll");
  EXPECT_EQ(reaffirmed[0].applied_bitrate_kbps, 20000);
  EXPECT_EQ(reaffirmed[0].applied_revision, request->revision);
  EXPECT_EQ(reaffirmed[0].callback_revision, 0u);
  EXPECT_EQ(entries_of(history, "encoder_applied").size(), 1u);
  EXPECT_NE(entries_of(history, "encoder_applied")[0].applied_revision, reaffirmed[0].applied_revision);
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, RepeatedUnchangedPollsFoldInsteadOfFillingTheHistory) {
  start_history_scope();
  apply_current_request(true);
  adaptive_bitrate::update_rtt(8.0);
  const auto before = history_now();
  for (int poll = 0; poll < 500; ++poll) {
    adaptive_bitrate::update_rtt(8.0);
    adaptive_bitrate::note_network_evidence_arrival(false);
    const auto request = adaptive_bitrate::get_live_bitrate_request();
    ASSERT_TRUE(request.has_value());
    adaptive_bitrate::acknowledge_live_bitrate_applied(request->revision, request->target_bitrate_kbps);
  }
  const auto after = history_now();
  EXPECT_EQ(after.dropped, 0u);
  EXPECT_LE(after.entries.size(), before.entries.size() + 1);
  const auto &last = after.entries.back();
  EXPECT_EQ(last.kind, "revision_reaffirmed");
  EXPECT_EQ(last.repeat_count, 500u);
  EXPECT_FALSE(last.fec_transition);  // ordinary repeated observations retain their existing folding
  EXPECT_GE(last.last_host_elapsed_ms, last.host_elapsed_ms);
  EXPECT_TRUE(last.input_available);
  EXPECT_EQ(last.input_rtt_ms, 8.0);
  EXPECT_TRUE(entries_of(after, "encoder_acknowledged").empty());
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, OverflowDropsTheOldestEntriesAndCountsThem) {
  start_history_scope();
  const auto start = history_now();
  for (int change = 0; change < 100; ++change) {
    adaptive_bitrate::set_live_bitrate(change % 2 == 0 ? 15000 : 16000);
  }
  const auto history = history_now();
  const auto recorded = start.entries.size() + 100;
  ASSERT_EQ(history.entries.size(), adaptive_bitrate::HISTORY_CAPACITY);
  EXPECT_EQ(history.capacity, adaptive_bitrate::HISTORY_CAPACITY);
  EXPECT_EQ(history.dropped, recorded - adaptive_bitrate::HISTORY_CAPACITY);
  for (std::size_t i = 1; i < history.entries.size(); ++i) {
    EXPECT_EQ(history.entries[i].sequence, history.entries[i - 1].sequence + 1);
  }
  EXPECT_EQ(history.entries.back().requested_bitrate_kbps, 16000);
  EXPECT_EQ(history.entries.front().sequence, history.entries.back().sequence - (adaptive_bitrate::HISTORY_CAPACITY - 1));
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, AnotherStreamScopeNeverInheritsEntries) {
  start_history_scope();
  apply_current_request(true);
  adaptive_bitrate::set_live_bitrate(15000);
  auto history = history_now();
  ASSERT_TRUE(history.scoped);
  EXPECT_EQ(history.session_generation, k_history_generation);
  const auto last_sequence = history.entries.back().sequence;

  // The same stream scope again keeps its history.
  adaptive_bitrate::set_session_scope(k_history_generation, "history-owner-a", false);
  EXPECT_EQ(history_now().entries.size(), history.entries.size());

  // Another owner of the same generation starts over.
  adaptive_bitrate::set_session_scope(k_history_generation, "history-owner-b", true);
  history = history_now();
  ASSERT_EQ(history.entries.size(), 1u);
  EXPECT_EQ(history.entries[0].kind, "scope_started");
  EXPECT_GT(history.entries[0].sequence, last_sequence);
  EXPECT_EQ(history.app_session_id, "history-owner-b");
  EXPECT_EQ(history.dropped, 0u);

  // A reset leaves the history unscoped, and an ended stream keeps nothing.
  adaptive_bitrate::reset();
  history = history_now();
  EXPECT_FALSE(history.scoped);
  ASSERT_EQ(history.entries.size(), 1u);
  EXPECT_EQ(history.entries[0].cause, "reset");
  adaptive_bitrate::set_session_scope(0, {}, false);
  history = history_now();
  EXPECT_FALSE(history.scoped);
  EXPECT_TRUE(history.entries.empty());
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, RecordingLeavesTheControllerSnapshotUnchanged) {
  start_history_scope();
  apply_current_request(true);
  adaptive_bitrate::set_live_bitrate(15000);
  const auto plain = adaptive_bitrate::get_state();
  const auto with_history = adaptive_bitrate::get_state_with_history();
  EXPECT_TRUE(plain.history.entries.empty());
  EXPECT_FALSE(with_history.history.entries.empty());
  EXPECT_EQ(plain.target_bitrate_kbps, with_history.target_bitrate_kbps);
  EXPECT_EQ(plain.applied_bitrate_kbps, with_history.applied_bitrate_kbps);
  EXPECT_EQ(plain.base_bitrate_kbps, with_history.base_bitrate_kbps);
  EXPECT_EQ(plain.state, with_history.state);
  EXPECT_EQ(plain.reason, with_history.reason);
  EXPECT_EQ(plain.active, with_history.active);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, with_history.history.entries.back().operator_revision);
  leave_history_clean();
}

TEST(AdaptiveBitrateHistory, ANewScopeWithoutAResetKeepsNothingRecordedBeforeIt) {
  start_history_scope();
  apply_current_request(true);
  adaptive_bitrate::update_rtt(9.0);

  // The stream ends, the controller keeps moving unscoped, and a new scope begins with no reset.
  adaptive_bitrate::set_session_scope(0, {}, false);
  adaptive_bitrate::set_live_bitrate(15000);
  adaptive_bitrate::set_live_bitrate(16000);
  EXPECT_TRUE(history_now().entries.size() >= 2u);
  EXPECT_FALSE(history_now().scoped);
  const auto before_scope = adaptive_bitrate::get_state();
  adaptive_bitrate::set_session_scope(k_history_generation + 1, "history-owner-c", true);

  const auto history = history_now();
  ASSERT_TRUE(history.scoped);
  EXPECT_EQ(history.session_generation, k_history_generation + 1);
  ASSERT_EQ(history.entries.size(), 1u);
  const auto &snapshot = history.entries[0];
  EXPECT_EQ(snapshot.kind, "scope_started");
  EXPECT_EQ(snapshot.cause, "initial_scope_snapshot");
  EXPECT_EQ(snapshot.requested_bitrate_kbps, 16000);
  EXPECT_EQ(snapshot.state, before_scope.state);
  EXPECT_EQ(snapshot.reason, before_scope.reason);
  // The previous scope's input is not this scope's input; the controller's own EWMA is untouched.
  EXPECT_FALSE(snapshot.input_available);
  EXPECT_EQ(adaptive_bitrate::get_state().ewma_rtt_ms, before_scope.ewma_rtt_ms);
  EXPECT_EQ(adaptive_bitrate::get_state().target_bitrate_kbps, before_scope.target_bitrate_kbps);

  // Only a later change is recorded after the snapshot.
  adaptive_bitrate::set_live_bitrate(16000);
  EXPECT_EQ(history_now().entries.size(), 1u);
  adaptive_bitrate::set_live_bitrate(17000);
  ASSERT_EQ(history_now().entries.size(), 2u);
  EXPECT_EQ(history_now().entries[1].requested_bitrate_kbps, 17000);
  leave_history_clean();
}

#ifdef __linux__
namespace {
  struct H2ControllerTrace {
    std::filesystem::path directory;
    std::unique_ptr<vrr_trace::recorder_t> recorder;
    H2ControllerTrace() {
      char scratch[] = "/tmp/polaris-h2-controller-XXXXXX";
      const auto *created = ::mkdtemp(scratch);
      if (!created) throw std::runtime_error("mkdtemp failed");
      directory = created;
      recorder = vrr_trace::recorder_t::open((directory / "trace.jsonl").string());
    }
    ~H2ControllerTrace() {
      leave_history_clean();
      if (recorder) recorder->stop();
      std::filesystem::remove_all(directory);
    }
    std::vector<nlohmann::json> rows(std::string_view type) {
      recorder->stop();
      std::ifstream in(directory / "trace.jsonl");
      std::vector<nlohmann::json> result;
      for (std::string line; std::getline(in,line);) {
        auto row = nlohmann::json::parse(line);
        if (row["type"] == type) result.push_back(std::move(row));
      }
      return result;
    }
  };
}

TEST(AdaptiveBitrateHistory, H2ScopeUsesExactOriginAndDistinguishesAnotherOwnerOfTheSameGeneration) {
  H2ControllerTrace trace;
  ASSERT_TRUE(trace.recorder);
  ASSERT_EQ(trace.recorder->status(),vrr_trace::status_e::running);
  vrr_trace::test_recorder_scope_t active(trace.recorder.get());
  const auto before = vrr_trace::ns(std::chrono::steady_clock::now());
  start_history_scope();
  const auto after = vrr_trace::ns(std::chrono::steady_clock::now());
  const auto first = history_now();
  ASSERT_TRUE(first.trace_scope); ASSERT_TRUE(first.trace_scope->origin_steady_ns);
  ASSERT_TRUE(first.trace_scope->first_sequence); ASSERT_FALSE(first.entries.empty());
  EXPECT_GE(first.trace_scope->origin_steady_ns,before); EXPECT_LE(first.trace_scope->origin_steady_ns,after);
  EXPECT_EQ(first.trace_scope->first_sequence,first.entries.front().sequence);
  adaptive_bitrate::set_session_scope(k_history_generation,"history-owner-a",false);
  EXPECT_EQ(history_now().trace_scope->id,first.trace_scope->id);
  EXPECT_EQ(history_now().trace_scope->origin_steady_ns,first.trace_scope->origin_steady_ns);
  adaptive_bitrate::set_session_scope(k_history_generation,"private-bearer-never-export",true);
  const auto other = history_now(); ASSERT_TRUE(other.trace_scope);
  EXPECT_GT(other.trace_scope->id,first.trace_scope->id);
  EXPECT_GT(other.trace_scope->first_sequence,first.trace_scope->first_sequence);
  adaptive_bitrate::set_session_scope(k_history_generation+1,"private-bearer-never-export",true);
  const auto replacement = history_now(); ASSERT_TRUE(replacement.trace_scope);
  EXPECT_GT(replacement.trace_scope->id,other.trace_scope->id);
  adaptive_bitrate::set_session_scope(0,{},false); EXPECT_FALSE(history_now().trace_scope);
  {
    vrr_trace::test_recorder_scope_t off(nullptr);
    adaptive_bitrate::set_session_scope(k_history_generation+2,"off-owner",true);
    EXPECT_FALSE(history_now().trace_scope);
    EXPECT_EQ(history_now().entries.front().kind,"scope_started");
  }
  const auto rows = trace.rows("live_tuning_scope"); ASSERT_EQ(rows.size(),3u);
  EXPECT_EQ(rows[0]["origin_steady_ns"],*first.trace_scope->origin_steady_ns);
  EXPECT_EQ(rows[0]["first_sequence"],*first.trace_scope->first_sequence);
  EXPECT_EQ(rows[1]["session_generation"],rows[0]["session_generation"]);
  EXPECT_NE(rows[1]["trace_scope"],rows[0]["trace_scope"]);
  EXPECT_EQ(rows[2]["session_generation"],k_history_generation+1);
  for (const auto &row : rows) {
    EXPECT_EQ(row.dump().find("private-bearer-never-export"),std::string::npos);
    EXPECT_FALSE(row.contains("app_session_id"));
  }
}

TEST(AdaptiveBitrateHistory, H2RealApplyRecreationAndSupersedeKeepObservedValuesAndOrder) {
  H2ControllerTrace trace;
  ASSERT_TRUE(trace.recorder);
  ASSERT_EQ(trace.recorder->status(),vrr_trace::status_e::running);
  vrr_trace::test_recorder_scope_t active(trace.recorder.get());
  start_history_scope();
  adaptive_bitrate::set_runtime_update_supported(true,"supported",20000);
  const auto initial = adaptive_bitrate::get_doctor_state();
  adaptive_bitrate::set_live_bitrate(15000);
  const auto apply = adaptive_bitrate::get_live_bitrate_request(); ASSERT_TRUE(apply);
  vrr_trace::bitrate_observation_t observation;
  int calls = 0;
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*apply,[&] { ++calls; return true; },&observation));
  EXPECT_EQ(calls,1); EXPECT_EQ(observation.applied_before_kbps,20000);
  EXPECT_EQ(observation.applied_before_revision,initial.revision);
  EXPECT_EQ(observation.requested_before_kbps,15000); EXPECT_EQ(observation.requested_before_revision,apply->revision);
  vrr_trace::record_bitrate_update(vrr_trace::event_t("bitrate_update").u("encoder",71),observation,
    apply->target_bitrate_kbps,apply->revision,20000,"applied",1);
  for (int i=0;i<3;++i) adaptive_bitrate::acknowledge_live_bitrate_applied(apply->revision,15000);
  adaptive_bitrate::set_live_bitrate(12000);
  const auto recreate = adaptive_bitrate::get_live_bitrate_request(); ASSERT_TRUE(recreate);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*recreate,[&] { ++calls; return false; },&observation));
  EXPECT_EQ(observation.applied_before_kbps,15000); EXPECT_EQ(observation.applied_before_revision,apply->revision);
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(recreate->revision,12000));
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps,0);
  vrr_trace::record_bitrate_update(vrr_trace::event_t("bitrate_update").u("encoder",71),observation,
    12000,recreate->revision,15000,"recreate",2);
  // Controller work may supersede that target before the replacement opens. No guessed rate.
  adaptive_bitrate::set_live_bitrate(11000);
  EXPECT_FALSE(adaptive_bitrate::apply_live_bitrate_request(*recreate,[&] { ++calls; return true; },&observation));
  EXPECT_EQ(calls,2); EXPECT_EQ(observation.requested_before_kbps,11000);
  EXPECT_EQ(observation.applied_before_kbps,0);
  vrr_trace::record_bitrate_update(vrr_trace::event_t("bitrate_update").u("encoder",71),observation,
    12000,recreate->revision,15000,"superseded",std::nullopt);
  EXPECT_FALSE(adaptive_bitrate::begin_live_bitrate_session_recreation(recreate->revision,12000));
  const auto rejected = adaptive_bitrate::get_live_bitrate_request(); ASSERT_TRUE(rejected);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*rejected,[&] { ++calls; return false; },&observation));
  vrr_trace::record_bitrate_update(vrr_trace::event_t("bitrate_update").u("encoder",71),observation,
    11000,rejected->revision,15000,"rejected",0);
  {
    vrr_trace::test_recorder_scope_t off(nullptr);
    const auto plain_before = adaptive_bitrate::get_doctor_state();
    ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*rejected,[&] { ++calls; return true; }));
    EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps,11000);
    EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision,plain_before.revision);
  }
  EXPECT_EQ(calls,4);
  const auto rows = trace.rows("bitrate_update"); ASSERT_EQ(rows.size(),4u);
  EXPECT_EQ(rows[0]["result"],"applied"); EXPECT_EQ(rows[0]["applied_before_revision"],initial.revision);
  EXPECT_EQ(rows[1]["result"],"recreate"); EXPECT_EQ(rows[1]["applied_before_kbps"],15000);
  EXPECT_EQ(rows[2]["result"],"superseded"); EXPECT_TRUE(rows[2]["backend_result"].is_null());
  EXPECT_EQ(rows[3]["result"],"rejected");
  for (std::size_t i=1;i<rows.size();++i) EXPECT_LT(rows[i-1]["order"],rows[i]["order"]);
}
#endif
