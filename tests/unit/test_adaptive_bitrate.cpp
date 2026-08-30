/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Unit tests for adaptive bitrate controller behavior.
 */

#include "src/adaptive_bitrate.h"
#include "src/config.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

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

TEST(AdaptiveBitrateController, ClampsBaseToConfiguredBounds) {
  enable_controller(100000);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(50000, state.base_bitrate_kbps);
  EXPECT_EQ(50000, state.target_bitrate_kbps);
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

TEST(AdaptiveBitrateController, NormalizesMaxBelowMinBeforeClampingBase) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 0;

  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_base_bitrate(30000);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_GE(state.max_bitrate_kbps, state.min_bitrate_kbps);
  EXPECT_EQ(state.min_bitrate_kbps, state.base_bitrate_kbps);
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
