/**
 * @file tests/unit/test_vrr_session_policy.cpp
 * @brief A variable-refresh request is an input, never an effective source-time claim.
 */
#include <src/vrr_session_policy.h>
#include <src/vrr_rtp_timestamp.h>
#include <chrono>
#include <limits>

#include <gtest/gtest.h>

#include <string_view>
#include <utility>
#include <vector>

using vrr_policy::admission_e;
using vrr_policy::requested_e;

TEST(VrrSessionPolicy, ParsesEachRequestFormAndRefusesToGuess) {
  using values_t = std::vector<std::string_view>;
  EXPECT_EQ(vrr_policy::parse_request(values_t {}), requested_e::absent);
  for (const auto on : {"1", "true", "TRUE", "True"}) {
    EXPECT_EQ(vrr_policy::parse_request(values_t {on}), requested_e::on) << on;
  }
  for (const auto off : {"0", "false", "FALSE"}) {
    EXPECT_EQ(vrr_policy::parse_request(values_t {off}), requested_e::off) << off;
  }
  for (const auto bad : {"", "2", "yes", " 1", "1 ", "on", "-1"}) {
    EXPECT_EQ(vrr_policy::parse_request(values_t {bad}), requested_e::malformed) << '"' << bad << '"';
  }
  // Two answers are no answer, even when they agree.
  EXPECT_EQ(vrr_policy::parse_request(values_t {"1", "1"}), requested_e::malformed);
  EXPECT_EQ(vrr_policy::parse_request(values_t {"1", "0"}), requested_e::malformed);
}

TEST(VrrSessionPolicy, WatchAndInputOnlySessionsNeverOwnTheCadence) {
  for (const auto &[watch, input] : {std::pair {true, false}, std::pair {false, true}, std::pair {true, true}}) {
    const auto record = vrr_policy::make_record(7, requested_e::on, admission_e::launch, watch, input);
    EXPECT_TRUE(record.watch_or_input_only);
    const auto decision = vrr_policy::decide(record, "source_driven");
    EXPECT_FALSE(decision.effective);
    EXPECT_EQ(decision.fallback_reason, "watch_or_input_only");
  }
  const auto owner = vrr_policy::make_record(8, requested_e::on, admission_e::resume, false, false);
  EXPECT_FALSE(owner.watch_or_input_only);
  EXPECT_EQ(owner.session_generation, 8u);
  EXPECT_EQ(owner.admission, admission_e::resume);
}

TEST(VrrSessionPolicy, FirstApplicableReasonWinsAndNothingIsEverEffective) {
  struct case_t {
    requested_e requested;
    std::string_view capture_pacing;
    std::string_view reason;
  };
  const case_t cases[] {
    {requested_e::malformed, "source_driven", "request_malformed"},
    {requested_e::absent, "source_driven", "not_requested"},
    {requested_e::off, "source_driven", "not_requested"},
    {requested_e::on, "", "route_unreported"},
    {requested_e::on, "unknown", "route_unreported"},
    {requested_e::on, "fixed_interval", "route_fixed_interval"},
    // The request plus a source_driven label is still no source-time claim: no clock, namespace or
    // encode-loop gate exists yet.
    {requested_e::on, "source_driven", "clock_domain_unverified"},
  };
  for (const auto &item : cases) {
    const auto record = vrr_policy::make_record(1, item.requested, admission_e::launch, false, false);
    const auto decision = vrr_policy::decide(record, item.capture_pacing);
    EXPECT_FALSE(decision.effective) << item.reason;
    EXPECT_EQ(decision.pacing_mode, "fixed_grid") << item.reason;
    EXPECT_EQ(decision.fallback_reason, item.reason);
  }
}

TEST(VrrSessionPolicy, HostPresentationCountsAreSeparateAndSaturate) {
  vrr_policy::host_presentation_counts_t host;
  host.count(vrr_policy::presentation_status_e::valid);
  host.count(vrr_policy::presentation_status_e::valid);
  host.count(vrr_policy::presentation_status_e::nonmonotonic);
  EXPECT_EQ(host[vrr_policy::presentation_status_e::valid], 2u);
  EXPECT_EQ(host[vrr_policy::presentation_status_e::nonmonotonic], 1u);
  EXPECT_EQ(host[vrr_policy::presentation_status_e::missing], 0u);
  host.counts[0] = UINT64_MAX;
  host.count(vrr_policy::presentation_status_e::valid);
  EXPECT_EQ(host[vrr_policy::presentation_status_e::valid], UINT64_MAX);
}

TEST(VrrSessionPolicy, ProductionReportingWithNoProvenRouteFallsBackAsRouteUnreported) {
  // Production passes no owner-route label today; a request then waits on route correlation.
  const auto record = vrr_policy::make_record(1, requested_e::on, admission_e::launch, false, false);
  const auto decision = vrr_policy::decide(record, {});
  EXPECT_FALSE(decision.effective);
  EXPECT_EQ(decision.fallback_reason, "route_unreported");
}

TEST(VrrSessionPolicy, ReportsOnlyKnownCapturePacingLabels) {
  EXPECT_EQ(vrr_policy::reported_capture_pacing("source_driven"), "source_driven");
  EXPECT_EQ(vrr_policy::reported_capture_pacing("fixed_interval"), "fixed_interval");
  EXPECT_EQ(vrr_policy::reported_capture_pacing(""), "unknown");
  EXPECT_EQ(vrr_policy::reported_capture_pacing("vrr"), "unknown");
}

TEST(VrrRtpTimestamp, SignedDeltasKeepProtocolWrapWithoutMovingTheSource) {
  EXPECT_EQ(vrr_pacing::rtp_timestamp(1'000'000'000, 0), 90000u);
  EXPECT_EQ(vrr_pacing::rtp_timestamp(0, 1'000'000'000), 0u - 90000u);
  EXPECT_EQ(vrr_pacing::rtp_timestamp(50'000, 0), 4u);  // ties to even
  EXPECT_EQ(vrr_pacing::rtp_timestamp(150'000, 0), 14u);
  EXPECT_EQ(vrr_pacing::rtp_timestamp(0, 50'000), 0u - 4u);
  EXPECT_FALSE(vrr_pacing::rtp_timestamp(INT64_MIN, 1));
  EXPECT_FALSE(vrr_pacing::rtp_timestamp(INT64_MAX, -1));
  EXPECT_TRUE(vrr_pacing::rtp_timestamp(INT64_MIN, 0));
  EXPECT_TRUE(vrr_pacing::rtp_timestamp(INT64_MAX, 0));
}

TEST(VrrRtpTimestamp, AgreesWithSignedChronoAcrossOrdinaryWireTimesAndWrap) {
  using ticks = std::chrono::duration<std::int64_t, std::ratio<1, 90000>>;
  for (std::int64_t ns = -100'000'000'000; ns <= 100'000'000'000; ns += 97'319'711) {
    const auto expected = std::chrono::round<ticks>(std::chrono::nanoseconds {ns}).count();
    ASSERT_EQ(vrr_pacing::rtp_timestamp(ns, 0), static_cast<std::uint32_t>(expected)) << ns;
  }
  const auto before = vrr_pacing::rtp_timestamp(47'721'858'844'444, 0);
  const auto after = vrr_pacing::rtp_timestamp(47'721'858'855'556, 0);
  ASSERT_TRUE(before); ASSERT_TRUE(after);
  EXPECT_EQ(std::uint32_t(*after - *before), 1u);
}
