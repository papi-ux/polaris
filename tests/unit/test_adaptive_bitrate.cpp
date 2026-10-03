/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Unit tests for adaptive bitrate controller behavior.
 */

#include "src/adaptive_bitrate.h"
#include "src/config.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <initializer_list>
#include <optional>
#include <thread>
#include <future>
#include <limits>
#include <vector>

using namespace std::chrono_literals;

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
}

TEST(AdaptiveBitrateController, ReducesTargetOnNetworkPressure) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ("network_pressure", state.state);
}

TEST(AdaptiveBitrateController, IgnoresSubthresholdRelativeRttSpikeOnFastLan) {
  enable_controller();

  // Build enough history to arm relative spike detection. The final sample is
  // three times the baseline, but 3 ms is not actionable network pressure.
  for (int i = 0; i < 6; ++i) {
    adaptive_bitrate::update_network_stats(0.0, 1.0);
  }
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(0.0, 3.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_NE(state.state, "network_pressure");
}

TEST(AdaptiveBitrateController, ReducesTargetOnActionableRelativeRttSpike) {
  enable_controller();

  for (int i = 0; i < 6; ++i) {
    adaptive_bitrate::update_network_stats(0.0, 8.0);
  }
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(0.0, 55.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.reason, "rtt_spike");
}

TEST(AdaptiveBitrateController, TelemetryMovementInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

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

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

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

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_observation = adaptive_bitrate::get_doctor_state();
  // Production publishes the host evidence epoch before feeding the adaptive
  // loop. No interval sleep is intentional: this covers the early-return path.
  adaptive_bitrate::note_network_evidence_arrival(true);
  adaptive_bitrate::update_network_stats(0.0, 55.0);

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

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.75, 0.05, 0.02, 3.0, 4.0, 28.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_EQ(state.base_bitrate_kbps, state.target_bitrate_kbps);
  EXPECT_EQ("frame_pacing_observed", state.state);
}

TEST(AdaptiveBitrateController, ReducesTargetOnEncoderPressure) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
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
    adaptive_bitrate::update_network_stats(0.0, 8.0);
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

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_LT(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
}

TEST(AdaptiveBitrateController, EncoderPressureInsideAdjustmentIntervalInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
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

  adaptive_bitrate::update_network_stats(0.0, 8.0);
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
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ("network_pressure", state.state);
  EXPECT_LT(state.target_bitrate_kbps, 161000);
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
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_LT(state.target_bitrate_kbps, 180000);
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

  adaptive_bitrate::update_network_stats(21.8, 12.0);

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

  adaptive_bitrate::update_network_stats(12.0, 120.0);
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
  adaptive_bitrate::update_network_stats(0.0, 8.0);
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
  adaptive_bitrate::update_network_stats(12.0, 120.0);
  adaptive_bitrate::update_stream_health(0.70, 0.10, 0.20, 8.0, 18.0, 50.0);

  const auto pending = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(pending.live_bitrate_kbps, 20000);
  EXPECT_EQ(pending.revision, *restore_revision);
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());

  adaptive_bitrate::acknowledge_live_bitrate_applied(*restore_revision, 20000);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(12.0, 120.0);

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

  // Above the floor, heavy loss cuts down to it and no further.
  enable_controller(100000);
  adaptive_bitrate::set_session_floor(90000, "pyrowave_advice");
  ASSERT_EQ(adaptive_bitrate::get_state().min_bitrate_kbps, 90000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  for (int i = 0; i < 2; ++i) {
    std::this_thread::sleep_for(1100ms);
    adaptive_bitrate::update_network_stats(20.0, 8.0);
  }
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.target_bitrate_kbps, 90000);

  // The next stream starts from adaptive_bitrate_min again.
  enable_controller(20000);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.min_bitrate_kbps, 2000);
  EXPECT_EQ(state.floor_source, "adaptive_bitrate_min");
}

namespace {
  // Live Tuning keeps 1.4.13's loss handling in this release. Every control ping reaches it as a reading
  // with 0% loss, about ten a second, and each client media report with its own loss. Every reading moves
  // its loss average by ewma_alpha, 0.3, and it acts on the first reading once a second is up: an average
  // above 1% cuts in proportion and one above 5% by the whole 20%, an RTT reading of 45 ms or more and
  // twice the running average cuts 10%, and it recovers only after 10 seconds with no lossy report.

  /// One client media report: the frames the client expected in its second, and how many never arrived.
  struct media_report_t {
    double expected;
    double lost;

    double loss_pct() const {
      return lost * 100.0 / expected;
    }
  };

  // The Retroid Pocket 6's recorded HEVC run, 3840x2160 at 120 fps over Wi-Fi: 9 of 122 frames lost and 9
  // of 121, in two of every eight reports, for five minutes, and the host's RTT through it. The same run
  // test_stream_stats.cpp replays through Doctor's judge.
  constexpr std::array<media_report_t, 8> k_rp6_hevc_reports {{
    {120, 0}, {122, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {121, 9}, {120, 0}
  }};
  constexpr std::array<double, 8> k_rp6_hevc_rtt_ms {4.0, 4.4, 10.1, 7.6, 9.6, 7.9, 8.6, 4.3};
  constexpr int k_rp6_hevc_kbps = 268988;

  struct live_second_t {
    int target_kbps = 0;
    /// Live Tuning's loss average when it acted, after the second's first reading.
    double loss_acted_on_pct = 0.0;
  };

  /// One second as Live Tuning hears it: ten control pings and, when there is one, the client's media
  /// report after `pings_before_report` of them. The second begins once a second has passed since the
  /// last one began, so its first reading is the one Live Tuning acts on.
  live_second_t live_second(std::optional<double> report_loss_pct, int pings_before_report, double rtt_ms = 8.0) {
    std::this_thread::sleep_for(1005ms);
    live_second_t second;
    for (int ping = 0; ping < 10; ++ping) {
      if (report_loss_pct && ping == pings_before_report) {
        adaptive_bitrate::update_network_stats(*report_loss_pct, rtt_ms);
        if (ping == 0) {
          second.loss_acted_on_pct = adaptive_bitrate::get_state().ewma_packet_loss;
        }
      }
      adaptive_bitrate::update_network_stats(0.0, rtt_ms);
      if (ping == 0 && !(report_loss_pct && pings_before_report == 0)) {
        second.loss_acted_on_pct = adaptive_bitrate::get_state().ewma_packet_loss;
      }
    }
    second.target_kbps = adaptive_bitrate::get_state().target_bitrate_kbps;
    return second;
  }

  /// Live Tuning's loss average after each of `readings`, fed back to back so none is acted on.
  std::vector<double> loss_average_after(std::initializer_list<double> readings) {
    std::vector<double> averages;
    for (const double loss : readings) {
      adaptive_bitrate::update_network_stats(loss, 8.0);
      averages.push_back(adaptive_bitrate::get_state().ewma_packet_loss);
    }
    return averages;
  }

  /// Leave no cut target behind for a later suite in this binary to read as a reduced stream.
  void leave_live_tuning_clean() {
    adaptive_bitrate::reset();
    adaptive_bitrate::set_enabled(false);
    config::video.adaptive_bitrate.enabled = false;
  }
}  // namespace

TEST(AdaptiveBitrateController, TheRecordedRp6HevcRunCutsNothingForItsLoss) {
  // The Retroid Pocket 6's HEVC run with its reports landing mid-second, five pings after each second
  // begins. A 7.4% report lifts the loss average to 2.2%, and the six pings before the next adjustment
  // take it under 0.3%, so no adjustment sees loss above 1% and the stream stays at 269 Mbps.
  enable_controller(k_rp6_hevc_kbps);
  adaptive_bitrate::update_network_stats(0.0, k_rp6_hevc_rtt_ms[0]);
  for (int ping = 0; ping < 6; ++ping) {
    adaptive_bitrate::update_network_stats(0.0, k_rp6_hevc_rtt_ms[ping % k_rp6_hevc_rtt_ms.size()]);
  }
  for (std::size_t second = 0; second < k_rp6_hevc_reports.size(); ++second) {
    const auto heard = live_second(k_rp6_hevc_reports[second].loss_pct(), 5, k_rp6_hevc_rtt_ms[second]);
    EXPECT_EQ(heard.target_kbps, k_rp6_hevc_kbps) << "second " << second;
    EXPECT_LT(heard.loss_acted_on_pct, 1.0) << "second " << second;
    EXPECT_NE(adaptive_bitrate::get_state().state, "network_pressure") << "second " << second;
  }
  leave_live_tuning_clean();
}

TEST(AdaptiveBitrateController, AnRp6ReportThatMeetsAnAdjustmentCutsForItsOwnLoss) {
  // Where the run's reports land decides whether 1.4.13 cuts for them. A 7.4% report keeps the loss
  // average above 1% for its own reading and the two pings after it and no longer, so an adjustment
  // that falls on one of those three cuts in proportion, under a tenth, and the others see none.
  enable_controller(k_rp6_hevc_kbps);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const double report = k_rp6_hevc_reports[6].loss_pct();
  const auto averages = loss_average_after({report, 0.0, 0.0, 0.0});
  EXPECT_NEAR(averages[0], 0.3 * report, 1e-9);
  EXPECT_GT(averages[1], 1.0);
  EXPECT_GT(averages[2], 1.0);
  EXPECT_LT(averages[3], 1.0);
  for (int ping = 0; ping < 30; ++ping) {
    adaptive_bitrate::update_network_stats(0.0, 8.0);
  }
  ASSERT_LT(adaptive_bitrate::get_state().ewma_packet_loss, 0.01);

  // The report is the first reading once the second is up.
  const auto heard = live_second(report, 0);
  EXPECT_GT(heard.loss_acted_on_pct, 1.0);
  EXPECT_LT(heard.target_kbps, k_rp6_hevc_kbps);
  EXPECT_GT(heard.target_kbps, k_rp6_hevc_kbps * 9 / 10);
  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.reason, "packet_loss");
  leave_live_tuning_clean();
}

TEST(AdaptiveBitrateController, ATwentyPercentBurstCutsWhereItsReportsLandLateInTheSecond) {
  // Three seconds that lose a fifth of their frames, each report landing two pings before the second
  // ends. The pings before the next adjustment leave the average near 2%, and each of the three
  // adjustments after a lossy report cuts in proportion. A 20% report keeps the average above 1% for
  // its own reading and five pings, so a burst whose reports land six readings or more before an
  // adjustment is under the line when Live Tuning acts, and one that lands on the adjustment is heavy
  // loss and cuts the whole 20%.
  enable_controller(k_rp6_hevc_kbps);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto averages = loss_average_after({20.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
  EXPECT_GT(averages[0], 5.0);
  EXPECT_GT(averages[5], 1.0);
  EXPECT_LT(averages[6], 1.0);
  for (int ping = 0; ping < 30; ++ping) {
    adaptive_bitrate::update_network_stats(0.0, 8.0);
  }
  ASSERT_LT(adaptive_bitrate::get_state().ewma_packet_loss, 0.01);

  std::vector<int> targets;
  for (int second = 0; second < 3; ++second) {
    targets.push_back(live_second(20.0, 8).target_kbps);
  }
  targets.push_back(live_second(0.0, 8).target_kbps);
  // The first lossy second's adjustment came before its report.
  EXPECT_EQ(targets[0], k_rp6_hevc_kbps);
  for (std::size_t i = 1; i < targets.size(); ++i) {
    EXPECT_LT(targets[i], targets[i - 1]) << i;
    EXPECT_GT(targets[i], targets[i - 1] * 9 / 10) << i;
  }
  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.reason, "packet_loss");
  leave_live_tuning_clean();
}

TEST(AdaptiveBitrateController, AnRttSpikeCutsTenPercentEachSecondItLasts) {
  // A Wi-Fi RTT spike to 60 ms on a stream that has read 8 ms, two seconds long, as ENet's estimate
  // stays up. Every ping and report carries it, and each second's adjustment cuts 10% with no loss at all.
  enable_controller(k_rp6_hevc_kbps);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  for (int ping = 0; ping < 200; ++ping) {
    adaptive_bitrate::update_network_stats(0.0, 8.0);
  }
  const auto first = live_second(0.0, 5, 60.0);
  EXPECT_EQ(first.target_kbps, static_cast<int>(k_rp6_hevc_kbps * 0.9));
  EXPECT_EQ(adaptive_bitrate::get_state().reason, "rtt_spike");
  const auto second = live_second(0.0, 5, 60.0);
  EXPECT_EQ(second.target_kbps, static_cast<int>(static_cast<int>(k_rp6_hevc_kbps * 0.9) * 0.9));
  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.reason, "rtt_spike");
  leave_live_tuning_clean();
}
