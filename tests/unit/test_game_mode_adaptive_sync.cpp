#include <src/game_mode_adaptive_sync.h>
#include <gtest/gtest.h>

using namespace game_mode_adaptive_sync;
using namespace std::chrono_literals;

namespace {
  observation_t active() {
    observation_t value;
    value.route_bound = value.generation_checked = value.amdgpu = true;
    value.generation = {12, 90, 1, 23, 100, 4};
    value.properties.capable = value.properties.feedback = {true, true, 32, 1, 0, 1};
    value.version.producer_exe = "/usr/bin/gamescope";
    value.version.os_release = {{"ID", "steamos"}, {"BUILD_ID", "fixture-build"}};
    value.observed_at = steady_clock_t::time_point {1s};
    value.generation_checked_at = steady_clock_t::time_point {3s};
    return value;
  }
  evaluation_t freeze_result(const store_t &store) {
    return evaluate(store.read(), steady_clock_t::time_point {4s}, {true, steady_clock_t::time_point {2s}});
  }
}

TEST(GameModeAdaptiveSyncTests, GMA1FreezeCorrelationDoesNotRequireCaptureProgressOrConfirmCurrentState) {
  store_t store;
  store.publish(active());
  const auto result = freeze_result(store);
  EXPECT_STREQ(result.advice, "qualified_correlation");
  EXPECT_STREQ(result.current_state, "unknown");
  EXPECT_STREQ(result.current_reason, "no_independent_freshness");
  EXPECT_EQ(result.age_ms, 3000);
}

TEST(GameModeAdaptiveSyncTests, GMA2ProducerRestartRetainsHistoryWithoutCorrelatingIt) {
  store_t store;
  store.publish(active());
  auto replaced = active();
  ++replaced.generation.start_time;
  replaced.properties.torn = true;
  store.publish(replaced);
  const auto result = freeze_result(store);
  EXPECT_EQ(result.last_state, last_state_e::active);
  EXPECT_FALSE(result.same_generation);
  EXPECT_STREQ(result.advice, "generic_freeze");
}

TEST(GameModeAdaptiveSyncTests, GenerationCheckBeforeSymptomOnsetCannotProveItStayedUnchangedUntilTheFreeze) {
  store_t store; auto value = active();
  value.generation_checked_at = steady_clock_t::time_point {1s}; store.publish(value);
  EXPECT_STREQ(freeze_result(store).advice, "generic_freeze");
}

TEST(GameModeAdaptiveSyncTests, GMA3ObservationDuringFreezeIsStillOnlyACorrelation) {
  store_t store;
  auto value = active(); value.observed_at = steady_clock_t::time_point {3s};
  store.publish(value);
  const auto result = freeze_result(store);
  EXPECT_STREQ(result.advice, "qualified_correlation");
  EXPECT_STREQ(result.current_state, "unknown");
  EXPECT_EQ(result.age_ms, 1000);
}

TEST(GameModeAdaptiveSyncTests, GMA4ActivePropertyDoesNotGradeHealthyOrProvideAdvice) {
  store_t store; store.publish(active());
  const auto result = evaluate(store.read(), steady_clock_t::time_point {4s});
  EXPECT_STREQ(result.advice, "none");
  EXPECT_STREQ(result.current_state, "unknown");
}

TEST(GameModeAdaptiveSyncTests, GMA5MissingObservationDoesNotBlockIndependentFreezeFinding) {
  store_t store;
  EXPECT_STREQ(freeze_result(store).advice, "generic_freeze");
  EXPECT_FALSE(freeze_result(store).last_state);
}

TEST(GameModeAdaptiveSyncTests, GMB1ZeroPairCannotProveUnavailableWithoutBoundConnector) {
  store_t store; auto value = active();
  value.properties.capable.value = value.properties.feedback.value = 0;
  EXPECT_STREQ(snapshot_reason(value), "connector_unbound");
  store.publish(value);
  EXPECT_FALSE(freeze_result(store).last_state);
  EXPECT_STREQ(freeze_result(store).current_state, "unknown");
}

TEST(GameModeAdaptiveSyncTests, GMB2InjectedConnectorPermitsUnsupportedHistoryButNoCurrentState) {
  store_t store; auto value = active(); value.connector_bound = true;
  value.properties.capable.value = value.properties.feedback.value = 0;
  store.publish(value);
  EXPECT_EQ(freeze_result(store).last_state, last_state_e::inactive_unsupported);
  EXPECT_STREQ(freeze_result(store).current_state, "unknown");
}

TEST(GameModeAdaptiveSyncTests, GMC1FeedbackTwoCannotBeTreatedAsActive) {
  store_t store; auto value = active(); value.properties.feedback.value = 2;
  EXPECT_STREQ(snapshot_reason(value), "property_out_of_domain");
  store.publish(value); EXPECT_FALSE(freeze_result(store).last_state);
}

TEST(GameModeAdaptiveSyncTests, GMC2CapabilityTwoCannotBeTreatedAsCapable) {
  auto value = active(); value.properties.capable.value = 2;
  EXPECT_STREQ(snapshot_reason(value), "property_out_of_domain");
}

TEST(GameModeAdaptiveSyncTests, GMC3ContradictionCannotChooseOnePropertyAsAuthority) {
  store_t store; auto value = active(); value.properties.capable.value = 0;
  EXPECT_STREQ(snapshot_reason(value), "contradictory_properties");
  store.publish(value); EXPECT_FALSE(freeze_result(store).last_state);
}

TEST(GameModeAdaptiveSyncTests, GMC4TornReadKeepsOriginalObservationAndAge) {
  store_t store; store.publish(active());
  auto value = active(); value.observed_at = steady_clock_t::time_point {3s};
  value.properties.feedback.value = 0; value.properties.torn = true;
  store.publish(value);
  EXPECT_EQ(freeze_result(store).last_state, last_state_e::active);
  EXPECT_EQ(freeze_result(store).age_ms, 3000);
}

TEST(GameModeAdaptiveSyncTests, GMC5PreferenceIsAbsentFromStateInputs) {
  store_t store; store.publish(active());
  // There is intentionally no preference field: no caller can mistake ENABLED for feedback.
  EXPECT_EQ(freeze_result(store).last_state, last_state_e::active);
}

TEST(GameModeAdaptiveSyncTests, GMD1VersionAndBuildRemainVerbatimAndUnmapped) {
  store_t store; auto value = active(); value.version.package = "fixture-package";
  store.publish(value);
  EXPECT_EQ(store.read().last_known->version.producer_exe, value.version.producer_exe);
  EXPECT_EQ(store.read().last_known->version.os_release, value.version.os_release);
  EXPECT_EQ(store.read().last_known->version.package, value.version.package);
  EXPECT_STREQ(freeze_result(store).range_status, "unmapped");
}

TEST(GameModeAdaptiveSyncTests, GMD2OperatorFixClaimCannotChangeVersionClassification) {
  store_t store; auto value = active(); value.version.operator_reports.push_back("fixed on preview");
  store.publish(value);
  EXPECT_EQ(store.read().last_known->version.operator_reports.size(), 1u);
  EXPECT_STREQ(freeze_result(store).range_status, "unmapped");
}

TEST(GameModeAdaptiveSyncTests, GMD3UnknownPackageDoesNotEraseRecordedExecutableAndBuild) {
  store_t store; store.publish(active());
  EXPECT_FALSE(store.read().last_known->version.package);
  EXPECT_STREQ(freeze_result(store).advice, "qualified_correlation");
}

TEST(GameModeAdaptiveSyncTests, GMD4MissingProvenanceCannotQualifyCompatibilityCorrelation) {
  store_t store; auto value = active(); value.version = {};
  store.publish(value);
  EXPECT_STREQ(freeze_result(store).advice, "generic_freeze");
}

TEST(GameModeAdaptiveSyncTests, ExactPropertySchemaRejectsMissingWrongTypeWidthCountAndTrailingItems) {
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto value = active();
    auto &property = value.properties.feedback;
    if (mutation == 0) property.present = false;
    if (mutation == 1) property.cardinal = false;
    if (mutation == 2) property.format = 16;
    if (mutation == 3) property.count = 2;
    if (mutation == 4) property.bytes_after = 4;
    store_t store; store.publish(value);
    EXPECT_FALSE(freeze_result(store).last_state) << mutation;
  }
}

TEST(GameModeAdaptiveSyncTests, RetiredOrUnboundObserverCannotOfferCorrelationOrBeRevived) {
  store_t store; store.publish(active()); store.end();
  auto later = active(); later.properties.feedback.value = 0;
  store.publish(later);
  EXPECT_EQ(freeze_result(store).last_state, last_state_e::active);
  EXPECT_FALSE(freeze_result(store).same_generation);
  EXPECT_STREQ(freeze_result(store).advice, "generic_freeze");
  store_t other; other.publish(active());
  later.route_bound = false; other.publish(later);
  EXPECT_FALSE(freeze_result(other).same_generation);
}

TEST(GameModeAdaptiveSyncTests, FutureObservationIsNotHistoryAndNonAmdgpuCannotQualify) {
  store_t store; auto value = active(); value.observed_at = steady_clock_t::time_point {10s};
  store.publish(value); EXPECT_FALSE(freeze_result(store).last_state);
  value = active(); value.amdgpu = false; store.publish(value);
  EXPECT_STREQ(freeze_result(store).advice, "generic_freeze");
}
