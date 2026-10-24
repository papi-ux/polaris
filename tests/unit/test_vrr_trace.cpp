/** @file tests/unit/test_vrr_trace.cpp
 * Version 2 producer controls. No display, encoder, network, device or physical acceptance claim.
 */
#include <src/vrr_trace.h>
#include <src/vrr_trace_clock.h>
#include <src/video.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>

static_assert(sizeof(vrr_trace::event_t) <= 4096, "hot observations have a finite value-copy budget");

#ifdef __linux__
  #include <sys/stat.h>
  #include <unistd.h>

namespace {
  using json = nlohmann::json;
  struct collection_t {
    std::filesystem::path dir;
    std::filesystem::path path;
    std::atomic<bool> hold {true};
    std::unique_ptr<vrr_trace::recorder_t> recorder;
    collection_t(vrr_trace::limits_t limits = {}) {
      char scratch[] = "/tmp/polaris-vrr-trace-XXXXXX";
      const auto *created = ::mkdtemp(scratch);
      if (!created) throw std::runtime_error("mkdtemp failed");
      dir = created; path = dir / "trace.jsonl";
      limits.test_hold_writer = &hold;
      recorder = vrr_trace::recorder_t::open(path.string(), limits);
    }
    ~collection_t() { if (recorder) recorder->stop(); std::filesystem::remove_all(dir); }
    void stop() { hold.store(false); recorder->stop(); }
    std::string bytes() const {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    std::vector<json> rows() const {
      std::vector<json> result;
      std::ifstream in(path); std::string line;
      while (std::getline(in, line)) result.push_back(json::parse(line));
      return result;
    }
    json receipt() const { std::ifstream in(path.string() + ".close.json"); return json::parse(in); }
  };

  std::string sha256(std::string_view input) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned size = 0;
    if (EVP_Digest(input.data(), input.size(), digest, &size, EVP_sha256(), nullptr) != 1) throw std::runtime_error("sha256 failed");
    std::string hex; const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < size; ++i) { hex += digits[digest[i] >> 4]; hex += digits[digest[i] & 15]; }
    return hex;
  }

  std::vector<json> select(const std::vector<json> &rows, std::string_view type) {
    std::vector<json> result;
    for (const auto &row : rows) if (row.at("type") == type) result.push_back(row);
    return result;
  }
}

TEST(VrrTraceTests, DefaultObserverIsOffAndDoesNotMintIdentities) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_EXIT({
    ::unsetenv("POLARIS_VRR_TRACE_V2");
    vrr_trace::start_from_environment();
    const bool off = !vrr_trace::enabled() && vrr_trace::status() == vrr_trace::status_e::off && vrr_trace::next_id() == 0;
    vrr_trace::record(vrr_trace::event_t("ignored"));
    ::_exit(off ? 0 : 1);
  }, ::testing::ExitedWithCode(0), "");
}

TEST(VrrTraceTests, ExplicitInvalidOptInReportsRefusalWithoutEnablingObserver) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_EXIT({
    ::setenv("POLARIS_VRR_TRACE_V2", "relative-path-refused", 1);
    vrr_trace::start_from_environment();
    ::_exit(!vrr_trace::enabled() && vrr_trace::status() == vrr_trace::status_e::refused && vrr_trace::refusal_errno() != 0 ? 0 : 1);
  }, ::testing::ExitedWithCode(0), "");
}

TEST(VrrTraceTests, FramingHashIdentityAndCloseAreIndependentFacts) {
  collection_t c;
  ASSERT_TRUE(c.recorder);
  ASSERT_EQ(c.recorder->status(), vrr_trace::status_e::running);
  c.recorder->record(vrr_trace::event_t("timing").u("identity", UINT64_MAX).n("missing_time").i("failed_clock", -1));
  c.stop();
  const auto rows = c.rows(); ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows.front()["schema"], "polaris.vrr-host-trace");
  EXPECT_EQ(rows.front()["major"], 2);
  EXPECT_TRUE(rows.front()["identity"].contains("mono_before_ns"));
  EXPECT_TRUE(rows.back()["identity"].contains("boottime_ns"));
  EXPECT_EQ(rows[1]["identity"].get<std::uint64_t>(), UINT64_MAX);
  EXPECT_TRUE(rows[1]["missing_time"].is_null()); EXPECT_EQ(rows[1]["failed_clock"], -1);
  EXPECT_EQ(rows[1]["order"], 1);
  EXPECT_TRUE(rows.back()["producer_complete"].get<bool>());
  EXPECT_EQ(rows.back()["attempts"], 1); EXPECT_EQ(rows.back()["event_rows"], 1);
  const auto bytes = c.bytes(); ASSERT_FALSE(bytes.empty()); EXPECT_EQ(bytes.back(), '\n');
  const auto before_footer = rows.back()["bytes_before_footer"].get<std::size_t>();
  EXPECT_EQ(rows.back()["content_sha256"], sha256(std::string_view(bytes).substr(0, before_footer)));
  const auto receipt = c.receipt();
  EXPECT_EQ(receipt["main_bytes"], bytes.size()); EXPECT_EQ(receipt["main_sha256"], sha256(bytes));
  EXPECT_TRUE(receipt["footer_written"].get<bool>()); EXPECT_EQ(receipt["write_errno"], 0); EXPECT_EQ(receipt["main_close_errno"], 0);
  struct stat st {}; ASSERT_EQ(::stat(c.path.c_str(), &st), 0); EXPECT_EQ(st.st_mode & 0777, 0600);
  ASSERT_EQ(::stat((c.path.string() + ".close.json").c_str(), &st), 0); EXPECT_EQ(st.st_mode & 0777, 0600);
}

TEST(VrrTraceTests, ExistingFileAndSymlinkComponentsRefuseWithoutTruncation) {
  collection_t c; c.stop();
  const auto original = c.bytes();
  auto existing = vrr_trace::recorder_t::open(c.path.string());
  ASSERT_TRUE(existing); EXPECT_EQ(existing->status(), vrr_trace::status_e::refused); EXPECT_EQ(c.bytes(), original);
  std::filesystem::create_symlink(c.path, c.dir / "link");
  auto leaf = vrr_trace::recorder_t::open((c.dir / "link").string());
  ASSERT_TRUE(leaf); EXPECT_EQ(leaf->status(), vrr_trace::status_e::refused); EXPECT_EQ(c.bytes(), original);
  std::filesystem::create_directory_symlink(c.dir, c.dir / "parent-link");
  auto parent = vrr_trace::recorder_t::open((c.dir / "parent-link" / "new").string());
  ASSERT_TRUE(parent); EXPECT_EQ(parent->status(), vrr_trace::status_e::refused); EXPECT_FALSE(std::filesystem::exists(c.dir / "new"));
  auto relative = vrr_trace::recorder_t::open("relative-trace");
  ASSERT_TRUE(relative); EXPECT_EQ(relative->status(), vrr_trace::status_e::refused);
}

TEST(VrrTraceTests, FullQueueEventCapAndInvalidEventAllRefuseCompleteness) {
  collection_t full;
  for (std::size_t i = 0; i < vrr_trace::k_queue_capacity + 1; ++i) full.recorder->record(vrr_trace::event_t("row").u("n", i));
  full.stop();
  const auto rows = full.rows(); const auto &footer = rows.back();
  EXPECT_EQ(footer["event_rows"], vrr_trace::k_queue_capacity); EXPECT_EQ(footer["lost_contention_or_full"], 1);
  EXPECT_FALSE(footer["producer_complete"].get<bool>());
  for (std::size_t i = 1; i + 1 < rows.size(); ++i) EXPECT_EQ(rows[i]["order"], i);

  vrr_trace::limits_t limits; limits.events = 2;
  collection_t capped(limits);
  vrr_trace::event_t overflow("overflow");
  for (std::size_t i = 0; i <= vrr_trace::k_fields; ++i) overflow.u("too_many", i);
  capped.recorder->record(overflow); capped.recorder->record(vrr_trace::event_t("ok")); capped.recorder->record(vrr_trace::event_t("capped"));
  capped.stop(); const auto end = capped.rows().back();
  EXPECT_EQ(end["invalid_event_loss"], 1); EXPECT_EQ(end["event_cap_loss"], 1); EXPECT_FALSE(end["producer_complete"].get<bool>());
}

TEST(VrrTraceTests, ConcurrentProducersKeepOneAdmissionOrderAndAccountForEveryAttempt) {
  collection_t c;
  std::vector<std::thread> producers;
  for (int producer = 0; producer < 4; ++producer) producers.emplace_back([&, producer] {
    for (int n = 0; n < 64; ++n) c.recorder->record(vrr_trace::event_t("concurrent").i("producer", producer).i("n", n));
  });
  for (auto &producer : producers) producer.join();
  c.stop(); const auto rows = c.rows(); const auto &footer = rows.back();
  EXPECT_EQ(footer["attempts"], 256);
  EXPECT_EQ(footer["admitted"].get<std::uint64_t>() + footer["lost_contention_or_full"].get<std::uint64_t>(), 256u);
  EXPECT_EQ(footer["event_rows"], footer["admitted"]);
  std::array<std::array<bool, 64>, 4> seen {};
  for (std::size_t i = 1; i + 1 < rows.size(); ++i) {
    const auto p = rows[i]["producer"].get<std::size_t>(), n = rows[i]["n"].get<std::size_t>();
    ASSERT_LT(p, 4u); ASSERT_LT(n, 64u); EXPECT_FALSE(seen[p][n]); seen[p][n] = true;
    EXPECT_EQ(rows[i]["order"], i);
  }
}

TEST(VrrTraceTests, ByteCapWriteFailureAndCloseFailureAreNotCleanClosures) {
  vrr_trace::limits_t small; small.bytes = 65536;
  collection_t capped(small); capped.recorder->record(vrr_trace::event_t("row")); capped.stop();
  EXPECT_EQ(capped.rows().back()["byte_cap_loss"], 1); EXPECT_FALSE(capped.rows().back()["producer_complete"].get<bool>());

  vrr_trace::limits_t write_fail; write_fail.test_write_failure = true;
  collection_t failed(write_fail); failed.stop();
  EXPECT_TRUE(failed.bytes().empty()); EXPECT_FALSE(failed.receipt()["footer_written"].get<bool>());
  EXPECT_NE(failed.receipt()["write_errno"], 0);

  vrr_trace::limits_t close_fail; close_fail.test_close_failure = true;
  collection_t closed(close_fail); closed.stop();
  EXPECT_TRUE(closed.rows().back()["producer_complete"].get<bool>()); // footer cannot know close's result
  EXPECT_NE(closed.receipt()["main_close_errno"], 0); // consumer must also require the receipt
}

TEST(VrrTraceTests, DelayedPtsJoinPreservesLegacyOrderAndExplainsCapacityAndRetirement) {
  collection_t c;
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    vrr_trace::submissions_t legacy;
    legacy.put(1, 101, 70); legacy.put(2, 102, 70);
    const auto second = legacy.take(2, 70, false); ASSERT_TRUE(second); EXPECT_EQ(second->id, 102u);
    const auto first = legacy.take(1, 70, false); ASSERT_TRUE(first); EXPECT_EQ(first->id, 101u); // out of order legacy output retains its own identity
    EXPECT_FALSE(legacy.take(3, 70, false)); // no zipping to a current call

    vrr_trace::submissions_t timed {"source_timing"};
    for (int i = 1; i <= 65; ++i) timed.put(i, 1000 + i, 71);
    EXPECT_FALSE(timed.take(1, 71, true)); // exact capacity eviction
    const auto matched = timed.take(3, 71, true); ASSERT_TRUE(matched); EXPECT_EQ(matched->id, 1003u);
    EXPECT_FALSE(timed.take(2, 71, true)); // ordered discard
    timed.clear(71, "encoder_retirement");
    // A source-paced table's newly inserted smallest PTS is itself the capacity victim.
    vrr_trace::submissions_t insertion;
    for (int i = 1; i <= 64; ++i) insertion.put(i, 2000 + i, 72);
    insertion.put(0, 2000, 72); EXPECT_FALSE(insertion.take(0, 72, true));
    EXPECT_TRUE(insertion.take(1, 72, true));
  }
  c.stop(); const auto rows = select(c.rows(), "submission_removed");
  EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const auto &r) { return r["reason"] == "capacity" && r["pts"] == 1; }));
  EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const auto &r) { return r["reason"] == "ordered_take_discard" && r["pts"] == 2; }));
  EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const auto &r) { return r["reason"] == "encoder_retirement" && r["encoder"] == 71; }));
}

TEST(VrrTraceTests, BatchFalseAndFallbackJoinKeepPartialProgressAndDuplicatesUnknown) {
  collection_t c;
  std::uint64_t batch_id = 0;
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    vrr_trace::send_attempt_t batch {900, 0, 65535, 2, "send_batch"}; batch_id = batch.id(); batch.finish(false);
    vrr_trace::send_attempt_t first {900, 0, 65535, 1, "send", batch_id}; first.finish(true);
    vrr_trace::send_attempt_t second {900, 0, 65536, 1, "send", batch_id}; second.finish(false);
    { vrr_trace::send_attempt_t exception {901, 0, 65535, 2, "send_batch"}; } // unreturned outcome, no guessed count
  }
  c.stop(); const auto rows = c.rows(); const auto attempts = select(rows, "send_attempt");
  ASSERT_EQ(attempts.size(), 4u); EXPECT_EQ(attempts[2]["first_sequence16"], 0);
  EXPECT_EQ(attempts[1]["retry_of"], batch_id); EXPECT_EQ(attempts[2]["retry_of"], batch_id);
  EXPECT_EQ(attempts[1]["duplicate_possible"], 1);
  const auto results = select(rows, "send_result"); ASSERT_EQ(results.size(), 4u);
  EXPECT_TRUE(results[0]["accepted_count"].is_null()); EXPECT_EQ(results[0]["partial_progress_unknown"], 1);
  EXPECT_EQ(results[1]["accepted_count"], 1); EXPECT_TRUE(results[2]["accepted_count"].is_null());
  EXPECT_TRUE(results[3]["api_result"].is_null()); EXPECT_TRUE(results[3]["accepted_count"].is_null());
}

TEST(VrrTraceTests, ClockProxyRecordsExactProofReadsWithoutAddingLeaseOrClockCalls) {
  class system_t final: public vrr_clock::system_t {
  public:
    int probes = 0;
    std::optional<peer_cred_t> peer_credentials(int) override { return peer_cred_t {4242, 1000}; }
    std::optional<vrr_clock::ns_id_t> time_namespace(std::int64_t) override { return vrr_clock::ns_id_t {3, 9}; }
    std::optional<std::uint64_t> process_starttime(std::int64_t) override { return 47; }
    std::uint32_t current_uid() override { return 1000; }
    vrr_clock::probe_sample_t sample_clocks() override { ++probes; return {10'000, 10'010, 10'020}; }
    std::int64_t monotonic_now_ns() override { return 9000; }
  } inner;
  class lease_t final: public vrr_clock::compositor_lease_t {
  public:
    lease_t(): compositor_lease_t({4242, 1000, 47}) {}
    mutable int calls = 0;
    bool current() const override { ++calls; return true; }
  };
  collection_t c;
  auto lease = std::make_shared<lease_t>();
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    vrr_trace::clock_system_t proxy(inner, 123);
    auto proof = vrr_clock::collect(5, 123, proxy, {}, lease);
    ASSERT_TRUE(proof->clock_domain_verified); EXPECT_EQ(inner.probes, 8); EXPECT_EQ(lease->calls, 2);
    proxy.sequence = 7; const auto live = proxy.sample_clocks(); EXPECT_EQ(live.steady_ns, 10'010);
  }
  c.stop(); const auto brackets = select(c.rows(), "clock_bracket"); ASSERT_EQ(brackets.size(), 9u);
  EXPECT_EQ(brackets.front()["source_sequence"], 0); EXPECT_EQ(brackets.back()["source_sequence"], 7);
  for (const auto &row : brackets) { EXPECT_EQ(row["epoch"], 123); EXPECT_EQ(row["mono_before_ns"], 10'000); EXPECT_EQ(row["mono_after_ns"], 10'020); }
}

TEST(VrrTraceTests, EncoderMoveTransfersOneRetirementAndPreservesSubmissionIdentity) {
  struct session_t: video::encode_session_t {
    int convert(video::frame_t &) override { return 0; }
    void request_idr_frame() override {}
    void request_normal_frame() override {}
    void invalidate_ref_frames(std::int64_t, std::int64_t) override {}
  };
  collection_t c;
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    session_t original; original.trace_encoder = 100; original.trace_submissions.put(1, 200, 100);
    {
      session_t moved {std::move(original)};
      EXPECT_EQ(original.trace_encoder, 0u); EXPECT_EQ(moved.trace_encoder, 100u);
    }
  }
  c.stop(); const auto rows = c.rows(); const auto retired = select(rows, "encoder_retired");
  ASSERT_EQ(retired.size(), 1u); EXPECT_EQ(retired[0]["encoder"], 100);
  const auto removed = select(rows, "submission_removed"); ASSERT_EQ(removed.size(), 1u);
  EXPECT_EQ(removed[0]["submission"], 200); EXPECT_EQ(removed[0]["reason"], "encoder_retirement");
}

TEST(VrrTraceTests, PacketRetirementRecordsQueueLossWithoutInventingASendReason) {
  collection_t c;
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    { video::packet_raw_generic packet {{}, 3, false}; packet.trace_packet = 300; packet.trace_enqueue_attempted = true; }
    { video::packet_raw_generic packet {{}, 4, false}; packet.trace_packet = 301; packet.trace_enqueue_attempted = true; packet.trace_dequeued = true; }
  }
  c.stop(); const auto retired = select(c.rows(), "packet_retired"); ASSERT_EQ(retired.size(), 2u);
  EXPECT_EQ(retired[0]["packet"], 300); EXPECT_EQ(retired[0]["dequeued"], 0);
  EXPECT_EQ(retired[0]["reason"], "not_dequeued_reason_unavailable");
  EXPECT_EQ(retired[1]["packet"], 301); EXPECT_EQ(retired[1]["dequeued"], 1);
}

TEST(VrrTraceTests, H2PolicyCopiesExactFieldCodesAndKeepsHistorySafeUnknown) {
  collection_t c;
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    vrr_trace::session_policy_t policy;
    policy.available = true; policy.requested_fps_milli = 119880; policy.resolved_fps_milli = 59940;
    {
      // Runtime strings expire before the writer reads the queued value-only observations.
      std::string display = "composed_display_components", fps = "runtime_policy";
      policy.display_mode_source = vrr_trace::policy_source(display);
      policy.target_fps_source = vrr_trace::policy_source(fps);
    }
    vrr_trace::record_session_policy(vrr_trace::event_t("session_policy").u("encoder",31).u("session_generation",41),policy);
    auto doctor = policy;
    doctor.resolved_fps_milli = 90000;
    doctor.display_mode_source = vrr_trace::policy_source("explicit_doctor_trial");
    doctor.target_fps_source = doctor.display_mode_source;
    vrr_trace::record_session_policy(vrr_trace::event_t("session_policy").u("encoder",35).u("session_generation",45),doctor);
    policy.display_mode_source = vrr_trace::policy_source("history_safe,explicit_doctor_trial,runtime_policy");
    policy.target_fps_source = vrr_trace::policy_source("future_unknown_source");
    vrr_trace::record_session_policy(vrr_trace::event_t("session_policy").u("encoder",32).u("session_generation",42),policy);
    vrr_trace::record_session_policy(vrr_trace::event_t("session_policy").u("encoder",33).u("session_generation",43),{});
    vrr_trace::test_recorder_scope_t off(nullptr);
    vrr_trace::record_session_policy(vrr_trace::event_t("session_policy").u("encoder",34),policy);
    vrr_trace::record_pacing_receipt({},false,{});
    vrr_trace::record_bitrate_update(vrr_trace::event_t("bitrate_update"),{},1000,1,2000,"rejected",0);
  }
  c.stop(); const auto rows = select(c.rows(),"session_policy"); ASSERT_EQ(rows.size(),4u);
  EXPECT_EQ(rows[0]["requested_client_fps_milli"],119880); EXPECT_EQ(rows[0]["session_target_fps_milli"],59940);
  EXPECT_EQ(rows[0]["display_mode_source_code"],static_cast<unsigned>(vrr_trace::policy_source_e::composed_display_components));
  EXPECT_EQ(rows[0]["target_fps_source_code"],static_cast<unsigned>(vrr_trace::policy_source_e::runtime_policy));
  EXPECT_TRUE(rows[0]["history_safe_target_fps_milli"].is_null()); EXPECT_TRUE(rows[0]["history_safe_source_code"].is_null());
  EXPECT_EQ(rows[0]["history_safe_at_resolver"],"not_consumed_by_deterministic_resolver");
  EXPECT_EQ(rows[1]["target_fps_source_code"],static_cast<unsigned>(vrr_trace::policy_source_e::explicit_doctor_trial));
  EXPECT_EQ(rows[1]["session_target_fps_milli"],90000); EXPECT_TRUE(rows[1]["history_safe_target_fps_milli"].is_null());
  EXPECT_EQ(rows[2]["display_mode_source_code"],0u); EXPECT_EQ(rows[2]["target_fps_source_code"],0u);
  EXPECT_EQ(rows[3]["policy_available"],0u); EXPECT_TRUE(rows[3]["session_target_fps_milli"].is_null());
  EXPECT_TRUE(rows[3]["history_safe_at_resolver"].is_null());
  EXPECT_TRUE(select(c.rows(),"pacing_receipt").empty()); EXPECT_TRUE(select(c.rows(),"bitrate_update").empty());
  EXPECT_EQ(c.rows().front()["minor"],vrr_trace::k_schema_minor);
  EXPECT_TRUE(c.rows().back()["producer_complete"].get<bool>());
}

TEST(VrrTraceTests, H2AcceptedReceiptWithoutSequenceRefusesCaptureCompleteness) {
  collection_t c;
  {
    vrr_trace::test_recorder_scope_t active(c.recorder.get());
    vrr_pacing::receipt_t receipt;
    receipt.session_generation = 41; receipt.event = vrr_pacing::event_e::readiness;
    vrr_trace::record_pacing_receipt(receipt,true,{});
  }
  c.stop(); const auto rows = c.rows(); const auto receipts = select(rows,"pacing_receipt");
  ASSERT_EQ(receipts.size(),1u); EXPECT_EQ(receipts[0]["accepted"],1u);
  EXPECT_TRUE(receipts[0]["accepted_sequence"].is_null());
  EXPECT_FALSE(receipts[0].contains("reason"));
  EXPECT_EQ(rows.back()["instrumentation_refusals"],1u);
  EXPECT_FALSE(rows.back()["producer_complete"].get<bool>());
}
#endif
