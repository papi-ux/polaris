/**
 * @file tests/unit/test_client_support_report.cpp
 * @brief Test paired-client support report intake.
 */
#include <chrono>
#include <gtest/gtest.h>
#include <src/client_support_report.h>
#include <string>

using namespace std::literals;

namespace {
  class ClientSupportReport: public ::testing::Test {
  protected:
    void SetUp() override {
      client_support_report::clear();
    }

    void TearDown() override {
      client_support_report::clear();
    }
  };

  std::string submission(const std::string &extra = R"("notes":"stream froze")") {
    return "{" + extra + R"(,"device":"Retroid Pocket 6","nova_version":"1.1.3","android_release":"13"})";
  }
}  // namespace

TEST_F(ClientSupportReport, AcceptsAReportAndRecordsHostSideFacts) {
  const auto parsed = client_support_report::parse_submission(
    submission(),
    "client-uuid-1",
    "2026-08-17T12:00:00Z"
  );

  ASSERT_EQ(parsed.status, client_support_report::accept_e::accepted);
  EXPECT_EQ(parsed.report.client_id, "client-uuid-1");
  EXPECT_EQ(parsed.report.received_at, "2026-08-17T12:00:00Z");
  EXPECT_EQ(parsed.report.device, "Retroid Pocket 6");
  EXPECT_EQ(parsed.report.notes, "stream froze");
}

TEST_F(ClientSupportReport, TakesIdentityFromTheCertificateAndNotTheBody) {
  // A paired client must not be able to file a report as a different client.
  const auto parsed = client_support_report::parse_submission(
    R"({"client_id":"someone-else","notes":"n"})",
    "client-uuid-1",
    "2026-08-17T12:00:00Z"
  );

  ASSERT_EQ(parsed.status, client_support_report::accept_e::accepted);
  EXPECT_EQ(parsed.report.client_id, "client-uuid-1");
}

TEST_F(ClientSupportReport, RejectsASubmissionOverTheSizeBound) {
  const std::string oversized = "{\"notes\":\"" + std::string(client_support_report::max_report_bytes + 10, 'x') + "\"}";

  const auto parsed = client_support_report::parse_submission(oversized, "c1", "t");

  EXPECT_EQ(parsed.status, client_support_report::accept_e::too_large);
  EXPECT_FALSE(parsed.error.empty());
}

TEST_F(ClientSupportReport, RejectsMalformedOrEmptySubmissions) {
  for (const auto body : {""sv, "not json"sv, "[]"sv, R"({"device":"RP6"})"sv}) {
    const auto parsed = client_support_report::parse_submission(body, "c1", "t");
    EXPECT_EQ(parsed.status, client_support_report::accept_e::malformed) << body;
  }
}

TEST_F(ClientSupportReport, TreatsAWronglyTypedFieldAsAbsentRatherThanFatal) {
  // A usable report with one odd field beats no report at all.
  const auto parsed = client_support_report::parse_submission(
    R"({"notes":"real note","device":12345,"crash":null})",
    "c1",
    "t"
  );

  ASSERT_EQ(parsed.status, client_support_report::accept_e::accepted);
  EXPECT_EQ(parsed.report.notes, "real note");
  EXPECT_TRUE(parsed.report.device.empty());
}

TEST_F(ClientSupportReport, BoundsAnOversizedFieldInsideAnAcceptedReport) {
  const std::string big = R"({"log_tail":")" + std::string(client_support_report::max_field_bytes + 500, 'y') + R"("})";

  const auto parsed = client_support_report::parse_submission(big, "c1", "t");

  ASSERT_EQ(parsed.status, client_support_report::accept_e::accepted);
  EXPECT_LT(parsed.report.log_tail.size(), client_support_report::max_field_bytes + 100);
  EXPECT_NE(parsed.report.log_tail.find("truncated by host"), std::string::npos);
}

TEST_F(ClientSupportReport, RateLimitsRepeatSubmissionsFromOneClient) {
  const auto now = std::chrono::steady_clock::now();

  EXPECT_TRUE(client_support_report::rate_limit_allows("c1", now));
  EXPECT_FALSE(client_support_report::rate_limit_allows("c1", now + 1s));
  // A different client is not punished for the first one's behaviour.
  EXPECT_TRUE(client_support_report::rate_limit_allows("c2", now + 1s));
  EXPECT_TRUE(client_support_report::rate_limit_allows("c1", now + client_support_report::min_submission_interval + 1s));
}

TEST_F(ClientSupportReport, ReplacesAClientsPreviousReportRatherThanStackingThem) {
  // One client repeating itself must not push every other client's report out.
  client_support_report::store({.client_id = "c1", .notes = "first"});
  client_support_report::store({.client_id = "c2", .notes = "other client"});
  client_support_report::store({.client_id = "c1", .notes = "second"});

  const auto held = client_support_report::recent();
  ASSERT_EQ(held.size(), 2);
  EXPECT_EQ(held[0].client_id, "c2");
  EXPECT_EQ(held[1].notes, "second");
}

TEST_F(ClientSupportReport, EvictsTheOldestOnceFull) {
  for (std::size_t index = 0; index < client_support_report::max_retained_reports + 3; ++index) {
    client_support_report::store({.client_id = "client-" + std::to_string(index), .notes = "n"});
  }

  const auto held = client_support_report::recent();
  ASSERT_EQ(held.size(), client_support_report::max_retained_reports);
  EXPECT_EQ(held.front().client_id, "client-3");
}

TEST_F(ClientSupportReport, RendersReportsForTheDiagnosticsApi) {
  client_support_report::store({
    .client_id = "c1",
    .device = "RP6",
    .nova_version = "1.1.3",
    .received_at = "2026-08-17T12:00:00Z",
    .notes = "stream froze",
  });

  const auto document = client_support_report::to_json();

  ASSERT_TRUE(document.is_array());
  ASSERT_EQ(document.size(), 1);
  EXPECT_EQ(document[0]["client_id"], "c1");
  EXPECT_EQ(document[0]["device"], "RP6");
  EXPECT_EQ(document[0]["notes"], "stream froze");
}

TEST_F(ClientSupportReport, RendersAnEmptyArrayWithNothingSubmitted) {
  const auto document = client_support_report::to_json();

  ASSERT_TRUE(document.is_array());
  EXPECT_TRUE(document.empty());
}

TEST_F(ClientSupportReport, StampsAUsableHostTimestamp) {
  const auto stamp = client_support_report::utc_timestamp_now();

  ASSERT_EQ(stamp.size(), 20);
  EXPECT_EQ(stamp.back(), 'Z');
  EXPECT_EQ(stamp[10], 'T');
}
