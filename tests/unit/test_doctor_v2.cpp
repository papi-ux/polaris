#include <gtest/gtest.h>

#include <src/doctor_v2.h>

#include <cstdlib>
#include <optional>
#include <limits>
#include <string>
#include <vector>

namespace {
  class doctor_v2_flag_t {
  public:
    explicit doctor_v2_flag_t(bool trials = false) {
      if (const char *value = std::getenv("POLARIS_DOCTOR_V2_SHADOW")) {
        previous_ = value;
        had_previous_ = true;
      }
      if (const char *value = std::getenv("POLARIS_DOCTOR_TRIALS")) {
        previous_trials_ = value;
        had_previous_trials_ = true;
      }
#ifdef _WIN32
      _putenv_s("POLARIS_DOCTOR_V2_SHADOW", "1");
      _putenv_s("POLARIS_DOCTOR_TRIALS", trials ? "1" : "");
#else
      setenv("POLARIS_DOCTOR_V2_SHADOW", "1", 1);
      if (trials) setenv("POLARIS_DOCTOR_TRIALS", "1", 1);
      else unsetenv("POLARIS_DOCTOR_TRIALS");
#endif
      doctor_v2::clear_for_tests();
    }

    ~doctor_v2_flag_t() {
      doctor_v2::clear_for_tests();
#ifdef _WIN32
      _putenv_s("POLARIS_DOCTOR_V2_SHADOW", had_previous_ ? previous_.c_str() : "");
      _putenv_s("POLARIS_DOCTOR_TRIALS", had_previous_trials_ ? previous_trials_.c_str() : "");
#else
      if (had_previous_) setenv("POLARIS_DOCTOR_V2_SHADOW", previous_.c_str(), 1);
      else unsetenv("POLARIS_DOCTOR_V2_SHADOW");
      if (had_previous_trials_) setenv("POLARIS_DOCTOR_TRIALS", previous_trials_.c_str(), 1);
      else unsetenv("POLARIS_DOCTOR_TRIALS");
#endif
    }

  private:
    std::string previous_;
    std::string previous_trials_;
    bool had_previous_ = false;
    bool had_previous_trials_ = false;
  };

  nlohmann::json sample(std::int64_t timestamp_ms,
                        std::int64_t index,
                        double received_fps = 120.0,
                        double rendered_fps = 120.0,
                        std::int64_t lost = 0) {
    return {{"sample", {
      {"monotonic_timestamp_ms", timestamp_ms},
      {"session_generation", 7},
      {"frames_expected", index * 120},
      {"frames_received", index * 120 - lost},
      {"frames_rendered", index * 120 - lost},
      {"frames_lost", lost},
      {"received_fps", received_fps},
      {"rendered_fps", rendered_fps},
      {"target_fps", 120.0},
      {"refresh_rate_hz", 120.0},
      {"rtt_ms", 8.0},
      {"decode_latency_ms", 2.0},
      {"width", 1920},
      {"height", 1080},
      {"bitrate_kbps", 40000},
      {"codec", "hevc"},
      {"topology", "desktop_display"},
      {"hdr", false}
    }}};
  }

  nlohmann::json host_evidence(double source_fps = 120.0, double duplicates = 0.0) {
    return {
      {"source_capture", {
        {"source_fps", source_fps},
        {"duplicate_frame_ratio", duplicates},
        {"capture_pacing", "measured"}
      }},
      {"encode", {
        {"encoded_fps", 120.0},
        {"encode_latency_ms", 3.0},
        {"target_fps", 120.0}
      }},
      {"transport", {{"bytes_sent", 1234567}, {"retransmissions", 0}}},
      {"receive_decode_render", {
        {"decoded_fps", 120.0},
        {"queue_latency_ms", 1.0},
        {"render_latency_ms", 1.0}
      }},
      {"effective_settings", {
        {"topology", "desktop_display"}, {"width", 1920}, {"height", 1080},
        {"codec", "hevc"}, {"hdr", false}, {"bitrate_kbps", 40000},
        {"target_fps", 120.0}, {"refresh_rate_hz", 120.0}
      }}
    };
  }

  nlohmann::json ingest_at(const std::string &owner,
                           const std::string &app,
                           const nlohmann::json &payload,
                           std::optional<std::int64_t> received_ms = std::nullopt) {
    const auto client_ms = payload.at("sample").value("monotonic_timestamp_ms", std::int64_t {0});
    doctor_v2::set_now_ms_for_tests(received_ms.value_or(client_ms));
    return doctor_v2::ingest(owner, app, payload);
  }
}

TEST(DoctorV2Tests, RejectsClientDiagnosisAndLaunchPolicy) {
  doctor_v2_flag_t enabled;
  auto payload = sample(1000, 1);
  payload["sample"]["actions"] = nlohmann::json::array();

  const auto result = ingest_at("owner", "app", payload);

  EXPECT_FALSE(result.at("status").get<bool>());
  EXPECT_EQ(result.at("code"), "invalid_evidence");
}

TEST(DoctorV2ProvenanceTests, UnavailableDecodeIsNotAMeasuredZero) {
  doctor_v2_flag_t enabled;
  auto payload = sample(1000, 1);
  payload["sample"].update({{"decode_latency_ms", 0.0}, {"decode_latency_available", false}});
  ASSERT_TRUE(ingest_at("owner", "app", payload).at("status").get<bool>());

  const auto status = doctor_v2::status("owner", "app", host_evidence());
  const auto &measurements = status.at("stages").at("receive_decode_render").at("measurements");
  EXPECT_TRUE(measurements.at("decode_latency_ms").is_null());
  EXPECT_FALSE(measurements.at("decode_latency_available").get<bool>());
  EXPECT_TRUE(status.at("trial_metrics").at("decode_latency_ms").is_null());
}

TEST(DoctorV2ProvenanceTests, LegacyMeasuredZeroRemainsAvailable) {
  doctor_v2_flag_t enabled;
  auto payload = sample(1000, 1);
  payload["sample"]["decode_latency_ms"] = 0.0;
  ASSERT_TRUE(ingest_at("owner", "app", payload).at("status").get<bool>());
  const auto status = doctor_v2::status("owner", "app", host_evidence());
  const auto &measurements = status.at("stages").at("receive_decode_render").at("measurements");
  EXPECT_EQ(measurements.at("decode_latency_ms"), 0.0);
  EXPECT_TRUE(measurements.at("decode_latency_available").get<bool>());
  EXPECT_EQ(measurements.at("frame_output_source"), "renderer_output");
}

TEST(DoctorV2ProvenanceTests, MissingOrMixedDecodeCannotBecomeACompleteMeasurement) {
  doctor_v2_flag_t enabled;
  auto first = sample(1000, 1);
  first["sample"]["decode_latency_ms"] = 4.0;
  ASSERT_TRUE(ingest_at("owner", "app", first).at("status").get<bool>());
  auto second = sample(2000, 2);
  second["sample"].erase("decode_latency_ms");
  ASSERT_TRUE(ingest_at("owner", "app", second).at("status").get<bool>());
  const auto status = doctor_v2::status("owner", "app", host_evidence());
  EXPECT_TRUE(status.at("stages").at("receive_decode_render").at("measurements").at("decode_latency_ms").is_null());
  EXPECT_TRUE(status.at("trial_metrics").at("decode_latency_ms").is_null());
}

TEST(DoctorV2ProvenanceTests, PresentRequestsRemainRawCountersAndCannotQualifyRendering) {
  doctor_v2_flag_t enabled(true);
  for (std::int64_t i = 0; i <= 75; ++i) {
    auto payload = sample(1000 + i * 1000, i, 120.0, 60.0);
    payload["sample"].update({{"decode_latency_ms", 0.0}, {"decode_latency_available", false},
                              {"frame_output_source", "present_request"}});
    ASSERT_TRUE(ingest_at("owner", "app", payload).at("status").get<bool>());
  }
  const auto status = doctor_v2::status("owner", "app", host_evidence());
  const auto &measurements = status.at("stages").at("receive_decode_render").at("measurements");
  EXPECT_EQ(measurements.at("frame_output_source"), "present_request");
  EXPECT_EQ(measurements.at("frame_output_fps"), 60.0);
  EXPECT_EQ(measurements.at("frames_output"), 75 * 120);
  EXPECT_TRUE(measurements.at("rendered_fps").is_null());
  EXPECT_TRUE(measurements.at("frames_rendered").is_null());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
  EXPECT_FALSE(status.at("trial_metrics").at("ready").get<bool>());
  EXPECT_TRUE(status.at("trial_metrics").at("rendered_fps").is_null());
  EXPECT_TRUE(status.at("trial_metrics").at("pacing_error_pct").is_null());
  for (const auto &hypothesis : status.at("hypotheses")) {
    EXPECT_NE(hypothesis.at("id"), "decoder_or_render_pressure");
    EXPECT_NE(hypothesis.at("id"), "source_or_capture_cadence");
  }
}

TEST(DoctorV2ProvenanceTests, RejectsMalformedAvailabilityAndOutputProvenance) {
  doctor_v2_flag_t enabled;
  for (const auto &value : std::vector<nlohmann::json> {nullptr, 0, "false", nlohmann::json::object()}) {
    auto payload = sample(1000, 1);
    payload["sample"]["decode_latency_available"] = value;
    const auto result = ingest_at("owner", "app", payload);
    EXPECT_FALSE(result.at("status").get<bool>()) << value;
    EXPECT_EQ(result.at("code"), "invalid_evidence") << value;
  }
  for (const auto &value : std::vector<nlohmann::json> {nullptr, true, 1, "scanout", "mixed"}) {
    auto payload = sample(1000, 1);
    payload["sample"]["frame_output_source"] = value;
    const auto result = ingest_at("owner", "app", payload);
    EXPECT_FALSE(result.at("status").get<bool>()) << value;
    EXPECT_EQ(result.at("code"), "invalid_evidence") << value;
  }
  auto missing = sample(1000, 1);
  missing["sample"].erase("decode_latency_ms");
  missing["sample"]["decode_latency_available"] = true;
  EXPECT_FALSE(ingest_at("owner", "app", missing).at("status").get<bool>());
}

TEST(DoctorV2ProvenanceTests, ExplicitMeasuredZeroAndRendererOutputRetainLegacyClassification) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i <= 75; ++i) {
    auto payload = sample(1000 + i * 1000, i);
    payload["sample"].update({{"decode_latency_ms", 0.0}, {"decode_latency_available", true},
                              {"frame_output_source", "renderer_output"}});
    ASSERT_TRUE(ingest_at("owner", "app", payload).at("status").get<bool>());
  }
  const auto status = doctor_v2::status("owner", "app", host_evidence());
  EXPECT_TRUE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("trial_metrics").at("decode_latency_ms"), 0.0);
  EXPECT_TRUE(status.at("trial_metrics").at("decode_latency_available").get<bool>());
  EXPECT_EQ(status.at("trial_metrics").at("rendered_fps"), 120.0);
}

TEST(DoctorV2ProvenanceTests, LatestPresentRequestCannotBorrowAnEarlierMeasuredWindow) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i < 75; ++i) {
    ASSERT_TRUE(ingest_at("owner", "app", sample(1000 + i * 1000, i)).at("status").get<bool>());
  }
  auto latest = sample(76000, 75);
  latest["sample"].update({{"decode_latency_ms", 0.0}, {"decode_latency_available", false},
                           {"frame_output_source", "present_request"}});
  ASSERT_TRUE(ingest_at("owner", "app", latest).at("status").get<bool>());
  const auto status = doctor_v2::status("owner", "app", host_evidence());
  const auto &measurements = status.at("stages").at("receive_decode_render").at("measurements");
  EXPECT_EQ(measurements.at("frame_output_source"), "mixed");
  EXPECT_TRUE(measurements.at("rendered_fps").is_null());
  EXPECT_TRUE(measurements.at("decode_latency_ms").is_null());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_FALSE(status.at("trial_metrics").at("ready").get<bool>());
}

TEST(DoctorV2ProvenanceTests, UnavailableWirePlaceholderStillRequiresAFiniteNonnegativeNumber) {
  doctor_v2_flag_t enabled;
  for (const auto &value : std::vector<nlohmann::json> {nullptr, "0", -1.0, std::numeric_limits<double>::infinity()}) {
    auto payload = sample(1000, 1);
    payload["sample"].update({{"decode_latency_available", false}, {"decode_latency_ms", value}});
    const auto result = ingest_at("owner", "app", payload);
    EXPECT_FALSE(result.at("status").get<bool>()) << value;
    EXPECT_EQ(result.at("code"), "invalid_evidence") << value;
  }
  auto ignored = sample(1000, 1);
  ignored["sample"].update({{"decode_latency_available", false}, {"decode_latency_ms", 12.0}});
  ASSERT_TRUE(ingest_at("owner", "app", ignored).at("status").get<bool>());
  EXPECT_TRUE(doctor_v2::status("owner", "app", host_evidence()).at("trial_metrics").at("decode_latency_ms").is_null());
}

TEST(DoctorV2ProvenanceTests, LegacyPyroWaveCpuReturnCannotBecomeMeasuredRendering) {
  doctor_v2_flag_t enabled;
  for (const char *codec : {"PyroWave", "pyrowave", " PyroWave ", "PYRO"}) {
    doctor_v2::clear_for_tests();
    for (std::int64_t i = 0; i <= 75; ++i) {
      auto payload = sample(1000 + i * 1000, i);
      payload["sample"].update({{"codec", codec}, {"decode_latency_ms", 7.0}});
      ASSERT_TRUE(ingest_at("owner", "app", payload).at("status").get<bool>());
    }
    const auto status = doctor_v2::status("owner", "app", host_evidence());
    const auto &measurements = status.at("stages").at("receive_decode_render").at("measurements");
    EXPECT_EQ(measurements.at("frame_output_source"), "present_request") << codec;
    EXPECT_TRUE(measurements.at("decode_latency_ms").is_null()) << codec;
    EXPECT_TRUE(measurements.at("rendered_fps").is_null()) << codec;
    EXPECT_FALSE(status.at("stable").get<bool>()) << codec;
  }
}

TEST(DoctorV2ProvenanceTests, TwoWindowsCannotBorrowTheSecondWindowsMeasuredProvenance) {
  doctor_v2_flag_t enabled(true);
  for (double output_fps : {60.0, 120.0}) {
    doctor_v2::clear_for_tests();
    for (std::int64_t i = 0; i <= 75; ++i) {
      auto payload = sample(1000 + i * 1000, i, output_fps, output_fps);
      if (i < 45) {
        payload["sample"].update({{"decode_latency_ms", 0.0}, {"decode_latency_available", false},
                                  {"frame_output_source", "present_request"}});
      }
      ASSERT_TRUE(ingest_at("owner", "app", payload).at("status").get<bool>());
    }
    const auto status = doctor_v2::status("owner", "app", host_evidence());
    ASSERT_TRUE(status.at("window").at("two_consecutive_complete").get<bool>());
    EXPECT_EQ(status.at("stages").at("receive_decode_render").at("measurements").at("frame_output_source"), "renderer_output");
    EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
    EXPECT_FALSE(status.at("stable").get<bool>());
    EXPECT_EQ(status.at("state"), "collecting");
    EXPECT_FALSE(status.at("actions_exposed").get<bool>());
    EXPECT_FALSE(status.at("trial_metrics").at("ready").get<bool>());
  }
}

TEST(DoctorV2Tests, RequiresMonotonicSamplesWithinGeneration) {
  doctor_v2_flag_t enabled;
  EXPECT_TRUE(ingest_at("owner", "app", sample(1000, 1)).at("status").get<bool>());

  const auto duplicate = ingest_at("owner", "app", sample(1000, 2));

  EXPECT_FALSE(duplicate.at("status").get<bool>());
  EXPECT_EQ(duplicate.at("code"), "non_monotonic_evidence");
}

TEST(DoctorV2Tests, NeedsWarmupAndTwoCoveredWindowsBeforeStable) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i <= 75; ++i) {
    ASSERT_TRUE(ingest_at("owner", "app", sample(1000 + i * 1000, i)).at("status").get<bool>());
  }

  const auto status = doctor_v2::status("owner", "app", host_evidence());

  EXPECT_EQ(status.at("state"), "classified");
  EXPECT_TRUE(status.at("window").at("two_consecutive_complete").get<bool>());
  EXPECT_TRUE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "undetermined");
  ASSERT_EQ(status.at("actions").size(), 1);
  EXPECT_FALSE(status.at("actions").front().at("exposed").get<bool>());
}

TEST(DoctorV2Tests, ClusteredSamplesCannotFakeWindowCoverage) {
  doctor_v2_flag_t enabled;
  for (std::int64_t second = 0; second <= 75; ++second) {
    for (std::int64_t duplicate = 0; duplicate < 2; ++duplicate) {
      ASSERT_TRUE(ingest_at(
        "owner", "app",
        sample(1000 + second * 1000 + duplicate, second * 2 + duplicate)
      ).at("status").get<bool>());
    }
  }
  const auto covered = doctor_v2::status("owner", "app", host_evidence());
  EXPECT_TRUE(covered.at("window").at("two_consecutive_complete").get<bool>());

  doctor_v2::clear_for_tests();
  for (std::int64_t i = 0; i < 60; ++i) {
    ASSERT_TRUE(ingest_at(
      "owner", "app", sample(1000 + i, i)
    ).at("status").get<bool>());
  }
  const auto clustered = doctor_v2::status("owner", "app", host_evidence());
  EXPECT_FALSE(clustered.at("window").at("two_consecutive_complete").get<bool>());
}

TEST(DoctorV2Tests, ClientTimestampsCannotSynthesizeEvidenceDuration) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i <= 75; ++i) {
    ASSERT_TRUE(ingest_at(
      "owner", "app", sample(1000 + i * 1000, i), 5000 + i
    ).at("status").get<bool>());
  }

  const auto status = doctor_v2::status("owner", "app", host_evidence());

  EXPECT_FALSE(status.at("window").at("two_consecutive_complete").get<bool>());
  EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("state"), "collecting");
}

TEST(DoctorV2Tests, CounterResetBeginsANewHostTimedEvidenceEpoch) {
  doctor_v2_flag_t enabled;
  ASSERT_TRUE(ingest_at("owner", "app", sample(1000, 100), 1000).at("status").get<bool>());

  const auto reset = ingest_at("owner", "app", sample(2000, 1), 2000);

  EXPECT_TRUE(reset.at("status").get<bool>());
  EXPECT_EQ(reset.at("state"), "counter_epoch_reset");
  EXPECT_EQ(reset.at("sample_count"), 1);
  const auto status = doctor_v2::status("owner", "app", host_evidence());
  EXPECT_FALSE(status.at("window").at("two_consecutive_complete").get<bool>());
}

TEST(DoctorV2Tests, StaticDuplicateContentIsNotAPacingFault) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i <= 75; ++i) {
    ASSERT_TRUE(ingest_at(
      "owner", "app", sample(1000 + i * 1000, i, 30.0, 30.0)
    ).at("status").get<bool>());
  }

  const auto status = doctor_v2::status("owner", "app", host_evidence(12.0, 0.80));

  EXPECT_TRUE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "undetermined");
  EXPECT_EQ(status.at("observations").back().at("id"), "static_or_duplicate_content");
}

TEST(DoctorV2Tests, MissingHostStagesCanNeverReportStable) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i <= 75; ++i) {
    ASSERT_TRUE(ingest_at("owner", "app", sample(1000 + i * 1000, i)).at("status").get<bool>());
  }

  const auto status = doctor_v2::status("owner", "app", nlohmann::json::object());

  EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("state"), "collecting");
}

TEST(DoctorV2Tests, EncoderWarningCanNeverAppearStable) {
  doctor_v2_flag_t enabled;
  for (std::int64_t i = 0; i <= 75; ++i) {
    ASSERT_TRUE(ingest_at("owner", "app", sample(1000 + i * 1000, i)).at("status").get<bool>());
  }
  auto evidence = host_evidence();
  evidence["encode"]["encoded_fps"] = 70.0;

  const auto status = doctor_v2::status("owner", "app", evidence);

  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "encoder_pressure");
}

TEST(DoctorV2Tests, ContainmentBuildCannotExposeTrialsThroughEnvironment) {
  doctor_v2_flag_t enabled(true);
  for (std::int64_t i = 0; i <= 75; ++i) {
    ASSERT_TRUE(ingest_at(
      "owner", "app", sample(1000 + i * 1000, i, 60.0, 60.0)
    ).at("status").get<bool>());
  }

  const auto status = doctor_v2::status("owner", "app", host_evidence());

  EXPECT_EQ(status.at("mode"), "shadow");
  EXPECT_FALSE(status.at("actions_exposed").get<bool>());
  ASSERT_EQ(status.at("actions").size(), 1);
  EXPECT_EQ(status.at("actions").front().at("id"), "recheck_pacing");
  EXPECT_EQ(status.at("actions").front().at("capability"), "recheck");
  EXPECT_FALSE(status.at("actions").front().at("exposed").get<bool>());
}

namespace {
  nlohmann::json source_time_evidence() {
    auto evidence = host_evidence(90.0);
    evidence["encode"]["encoded_fps"] = 90.0;
    evidence["vrr"] = {
      {"schema", 1}, {"session_generation", 7}, {"requested", "true"}, {"requested_from", "launch"},
      {"effective", true}, {"pacing_mode", "source_time"}, {"fallback_reason", nullptr},
      {"capture_pacing", "source_driven"}
    };
    evidence["source_cadence"] = {
      {"schema", 1}, {"session_generation", 7}, {"clock_domain_verified", true}, {"clock_epoch_id", "test-clock-epoch"},
      {"window_start_ms", 46000}, {"window_end_ms", 75000}, {"source_frames", 2611}, {"encoded_source_frames", 2611},
      {"first_source_ns", 46000000000LL}, {"last_source_ns", 75000000000LL},
      {"invalid_source_frames", 0}, {"routine_repeat_frames", 0}, {"source_fps", 90.0},
      {"ceiling_dropped_source_frames", 0},
      {"max_source_gap_ms", 20.0}, {"max_delivery_interval_error_ms", 0.2}, {"timestamp_error_ms", 0.005},
      {"limits", {{"max_source_gap_ms", 25.0}, {"max_delivery_interval_error_ms", 2.0}, {"max_timestamp_error_ms", 0.01}}}
    };
    return evidence;
  }

  void covered_samples(double received = 90.0, double rendered = 90.0, bool loss = false) {
    for (std::int64_t i = 0; i <= 75; ++i) {
      ASSERT_TRUE(ingest_at("owner", "app", sample(1000 + i * 1000, i, received, rendered, loss ? i * 6 : 0))
        .at("status").get<bool>());
    }
  }

  bool has_hypothesis(const nlohmann::json &status, const char *id) {
    for (const auto &hypothesis : status.at("hypotheses")) {
      if (hypothesis.at("id") == id) return true;
    }
    return false;
  }
}

TEST(DoctorV2CadenceTests, ObservedNinetyFpsSourceDoesNotFailAgainstA120Ceiling) {
  doctor_v2_flag_t enabled;
  covered_samples();
  const auto status = doctor_v2::status("owner", "app", source_time_evidence());
  EXPECT_TRUE(status.at("stable").get<bool>());
  EXPECT_FALSE(has_hypothesis(status, "encoder_pressure"));
  EXPECT_FALSE(has_hypothesis(status, "source_or_capture_cadence"));
  EXPECT_FALSE(status.at("trial_metrics").at("ready").get<bool>());
  EXPECT_TRUE(status.at("trial_metrics").at("pacing_error_pct").is_null());
}

TEST(DoctorV2CadenceTests, ARequestAndSourceDrivenLabelKeepFixedGridSemantics) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["vrr"].update({{"effective", false}, {"pacing_mode", "fixed_grid"}, {"fallback_reason", "clock_domain_unverified"}});
  evidence.erase("source_cadence");
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_TRUE(has_hypothesis(status, "encoder_pressure"));
  EXPECT_TRUE(has_hypothesis(status, "source_or_capture_cadence"));
}

TEST(DoctorV2CadenceTests, ContradictoryFixedGridFallbacksCannotBecomeStable) {
  doctor_v2_flag_t enabled;
  covered_samples(120.0, 120.0);
  const std::vector<nlohmann::json> policies {
    {{"requested", "true"}, {"capture_pacing", "unknown"}, {"fallback_reason", "not_requested"}},
    {{"requested", "absent"}, {"capture_pacing", "unknown"}, {"fallback_reason", "request_malformed"}},
    {{"requested", "false"}, {"capture_pacing", "fixed_interval"}, {"fallback_reason", "clock_domain_unverified"}},
    {{"requested", "true"}, {"capture_pacing", "source_driven"}, {"fallback_reason", "route_fixed_interval"}},
    {{"requested", "true"}, {"capture_pacing", "fixed_interval"}, {"fallback_reason", "route_unreported"}},
    {{"requested", "malformed"}, {"capture_pacing", "unknown"}, {"fallback_reason", "not_requested"}},
    {{"requested", "false"}, {"capture_pacing", "source_driven"}, {"fallback_reason", "presentation_time_missing"}}
  };
  for (const auto &policy : policies) {
    auto evidence = host_evidence(120.0);
    evidence["vrr"] = source_time_evidence()["vrr"];
    evidence["vrr"].update({{"effective", false}, {"pacing_mode", "fixed_grid"}});
    evidence["vrr"].update(policy);
    const auto status = doctor_v2::status("owner", "app", evidence);
    EXPECT_EQ(status.at("state"), "collecting") << policy.dump();
    EXPECT_FALSE(status.at("stable").get<bool>()) << policy.dump();
    EXPECT_FALSE(status.at("coverage_adequate").get<bool>()) << policy.dump();
  }
}

TEST(DoctorV2CadenceTests, ValidFoundationPoliciesRetainFixedGridAndGenerationRecovery) {
  doctor_v2_flag_t enabled;
  covered_samples(120.0, 120.0);
  const std::vector<nlohmann::json> policies {
    {{"requested", "absent"}, {"fallback_reason", "not_requested"}},
    {{"requested", "false"}, {"fallback_reason", "not_requested"}},
    {{"requested", "malformed"}, {"fallback_reason", "request_malformed"}},
    {{"requested", "true"}, {"fallback_reason", "route_unreported"}},
    {{"requested", "malformed"}, {"fallback_reason", "watch_or_input_only"}},
    {{"requested", "true"}, {"fallback_reason", "watch_or_input_only"}}
  };
  for (const auto &policy : policies) {
    auto evidence = host_evidence(120.0);
    evidence["vrr"] = source_time_evidence()["vrr"];
    evidence["vrr"].update({{"effective", false}, {"pacing_mode", "fixed_grid"}, {"capture_pacing", "unknown"}});
    evidence["vrr"].update(policy);
    evidence["vrr"]["session_generation"] = 8;
    EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("coverage_adequate").get<bool>());
    evidence["vrr"]["session_generation"] = 7;
    EXPECT_TRUE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>()) << policy.dump();
  }
}

TEST(DoctorV2CadenceTests, SourceStallsAtEitherWindowEdgeRemainVisible) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const char *edge : {"first_source_ns", "last_source_ns"}) {
    auto evidence = source_time_evidence();
    // Keep all counts and interior intervals plausible while placing a 400 ms
    // freeze outside them. Source FPS alone cannot reveal the affected edge.
    evidence["source_cadence"][edge] = edge == std::string("first_source_ns") ? 46400000000LL : 74600000000LL;
    evidence["source_capture"]["duplicate_frame_ratio"] = 0.9;
    const auto status = doctor_v2::status("owner", "app", evidence);
    EXPECT_FALSE(status.at("stable").get<bool>()) << edge;
    EXPECT_EQ(status.at("primary_issue"), "source_or_capture_cadence") << edge;
  }
}

TEST(DoctorV2CadenceTests, MissingMalformedOrOutOfWindowSourceEdgesCannotQualify) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const char *edge : {"first_source_ns", "last_source_ns"}) {
    auto missing = source_time_evidence();
    missing["source_cadence"].erase(edge);
    EXPECT_FALSE(doctor_v2::status("owner", "app", missing).at("coverage_adequate").get<bool>()) << edge;
    for (const nlohmann::json &value : std::vector<nlohmann::json> {
           true, 46000000000.0, -1, 45999999999LL, 75000000001LL, UINT64_MAX}) {
      auto evidence = source_time_evidence();
      evidence["source_cadence"][edge] = value;
      EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("coverage_adequate").get<bool>()) << edge << value;
    }
  }
  auto reversed = source_time_evidence();
  reversed["source_cadence"].update({{"first_source_ns", 50000000000LL}, {"last_source_ns", 49000000000LL}});
  EXPECT_FALSE(doctor_v2::status("owner", "app", reversed).at("coverage_adequate").get<bool>());
}

TEST(DoctorV2CadenceTests, AnInteriorGapCannotExceedItsSourceSpan) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["max_source_gap_ms"] = 30000.0;
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_EQ(status.at("state"), "collecting");
  EXPECT_FALSE(status.at("stable").get<bool>());
}

TEST(DoctorV2CadenceTests, ASourceAboveTheCeilingNeedsVerifiedDropAccounting) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["source_fps"] = 180.0;
  evidence["source_cadence"]["source_frames"] = 5221;
  evidence["source_cadence"]["max_source_gap_ms"] = 10.0;
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_EQ(status.at("state"), "collecting");
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_FALSE(status.at("trial_metrics").at("ready").get<bool>());
  EXPECT_FALSE(has_hypothesis(status, "encoder_pressure"));
  EXPECT_FALSE(has_hypothesis(status, "source_or_capture_cadence"));
}

TEST(DoctorV2CadenceTests, AQuietWindowEdgeCannotHideAFasterSourceSpan) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  // Window average is 90 FPS, but 2611 sources over this 17 s active span
  // exceed the 120 FPS ceiling. Legitimate downselection is not encoder loss.
  evidence["source_cadence"].update({{"first_source_ns", 52000000000LL}, {"last_source_ns", 69000000000LL},
                                    {"max_source_gap_ms", 7.0}, {"encoded_source_frames", 2040}});
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_EQ(status.at("state"), "collecting");
  EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
  EXPECT_FALSE(has_hypothesis(status, "encoder_pressure"));
  EXPECT_FALSE(has_hypothesis(status, "source_or_capture_cadence"));
}

TEST(DoctorV2CadenceTests, CeilingDropsOrAnUnknownLedgerCannotBlameEncoding) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto missing = source_time_evidence();
  missing["source_cadence"].erase("ceiling_dropped_source_frames");
  EXPECT_FALSE(doctor_v2::status("owner", "app", missing).at("coverage_adequate").get<bool>());
  // A burst can be hidden by both means. The explicit ledger stays required
  // even for their otherwise qualified 90 FPS window and source span.
  for (const nlohmann::json &value : std::vector<nlohmann::json> {1, 611, true, 0.0, -1, UINT64_MAX}) {
    auto evidence = source_time_evidence();
    evidence["source_cadence"]["encoded_source_frames"] = 2000;
    evidence["source_cadence"]["ceiling_dropped_source_frames"] = value;
    const auto status = doctor_v2::status("owner", "app", evidence);
    EXPECT_EQ(status.at("state"), "collecting") << value;
    EXPECT_FALSE(status.at("stable").get<bool>()) << value;
    EXPECT_FALSE(has_hypothesis(status, "encoder_pressure")) << value;
  }
}

TEST(DoctorV2CadenceTests, MissingObservedCadenceCannotBecomeHealthy) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence.erase("source_cadence");
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
  EXPECT_EQ(status.at("state"), "collecting");
}

TEST(DoctorV2CadenceTests, GenerationMismatchCannotBorrowAnotherStreamsMeasurements) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const char *object : {"vrr", "source_cadence"}) {
    auto evidence = source_time_evidence();
    evidence[object]["session_generation"] = 8;
    const auto status = doctor_v2::status("owner", "app", evidence);
    EXPECT_FALSE(status.at("stable").get<bool>()) << object;
    EXPECT_FALSE(status.at("coverage_adequate").get<bool>()) << object;
  }
}

TEST(DoctorV2CadenceTests, TimestampOrGenerationFloatsCannotQualify) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const char *key : {"session_generation", "window_start_ms", "source_frames"}) {
    auto evidence = source_time_evidence();
    evidence["source_cadence"][key] = evidence["source_cadence"][key].get<double>();
    EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>()) << key;
  }
}

TEST(DoctorV2CadenceTests, ABooleanCannotImpersonateANumericPolicySchema) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["vrr"]["schema"] = true;
  EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>());
}

TEST(DoctorV2CadenceTests, SourceAndClientWindowsMustMatchDuringARateChange) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["window_start_ms"] = 74000;
  evidence["source_cadence"]["source_frames"] = 91;
  evidence["source_cadence"]["encoded_source_frames"] = 91;
  EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("coverage_adequate").get<bool>());
}

TEST(DoctorV2CadenceTests, StaleHostTimingNeverQualifiesEvenWithCompleteClientHistory) {
  doctor_v2_flag_t enabled;
  covered_samples();
  doctor_v2::set_now_ms_for_tests(80000);
  const auto status = doctor_v2::status("owner", "app", source_time_evidence());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_FALSE(status.at("coverage_adequate").get<bool>());
}

TEST(DoctorV2CadenceTests, FutureHostWindowNeverQualifies) {
  doctor_v2_flag_t enabled;
  covered_samples();
  doctor_v2::set_now_ms_for_tests(74000);
  EXPECT_FALSE(doctor_v2::status("owner", "app", source_time_evidence()).at("stable").get<bool>());
}

TEST(DoctorV2CadenceTests, SourceClockMustBeVerifiedIndependentlyOfTheRequest) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["clock_domain_verified"] = false;
  EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>());
}

TEST(DoctorV2CadenceTests, RealSourceFreezeStaysVisible) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["max_source_gap_ms"] = 200.0;
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "source_or_capture_cadence");
}

TEST(DoctorV2CadenceTests, DeliveryJitterIsSeparateFromIntentionalSourceVariation) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["max_delivery_interval_error_ms"] = 10.0;
  const auto status = doctor_v2::status("owner", "app", evidence);
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "delivery_jitter");
  EXPECT_FALSE(has_hypothesis(status, "source_or_capture_cadence"));
}

TEST(DoctorV2CadenceTests, TimestampSnappingRemainsARealCadenceFault) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["timestamp_error_ms"] = 2.0;
  EXPECT_EQ(doctor_v2::status("owner", "app", evidence).at("primary_issue"), "source_or_capture_cadence");
}

TEST(DoctorV2CadenceTests, ConfirmedLossIsNotHiddenByHealthySourceTiming) {
  doctor_v2_flag_t enabled;
  covered_samples(90.0, 90.0, true);
  const auto status = doctor_v2::status("owner", "app", source_time_evidence());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "network_pressure");
}

TEST(DoctorV2CadenceTests, DecoderPressureIsNotHiddenBySourceTiming) {
  doctor_v2_flag_t enabled;
  covered_samples(90.0, 85.0);
  const auto status = doctor_v2::status("owner", "app", source_time_evidence());
  EXPECT_FALSE(status.at("stable").get<bool>());
  EXPECT_EQ(status.at("primary_issue"), "decoder_or_render_pressure");
}

TEST(DoctorV2CadenceTests, EncoderDropPressureRemainsVisible) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["encode"]["dropped_frame_ratio"] = 0.1;
  EXPECT_EQ(doctor_v2::status("owner", "app", evidence).at("primary_issue"), "encoder_pressure");
}

TEST(DoctorV2CadenceTests, MissingEncodedSourceFramesArePressureWithoutATargetFpsComparison) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_cadence"]["encoded_source_frames"] = 2000;
  EXPECT_EQ(doctor_v2::status("owner", "app", evidence).at("primary_issue"), "encoder_pressure");
}

TEST(DoctorV2CadenceTests, BadSourceSamplesAndRoutineRepeatsCannotQualify) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const char *key : {"invalid_source_frames", "routine_repeat_frames"}) {
    auto evidence = source_time_evidence();
    evidence["source_cadence"][key] = 1;
    EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>()) << key;
  }
}

TEST(DoctorV2CadenceTests, NonfiniteOrInconsistentMeasurementsCannotQualify) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const auto value : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), -1.0, 10.0}) {
    auto evidence = source_time_evidence();
    evidence["source_cadence"]["source_fps"] = value;
    EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>());
  }
}

TEST(DoctorV2CadenceTests, UnknownDiagnosticVersionsFailClosed) {
  doctor_v2_flag_t enabled;
  covered_samples();
  for (const char *object : {"vrr", "source_cadence"}) {
    auto evidence = source_time_evidence();
    evidence[object]["schema"] = 2;
    EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>()) << object;
  }
}

TEST(DoctorV2CadenceTests, StaticDuplicateHintsCannotHideASourceFreeze) {
  doctor_v2_flag_t enabled;
  covered_samples();
  auto evidence = source_time_evidence();
  evidence["source_capture"]["duplicate_frame_ratio"] = 0.9;
  evidence["source_cadence"]["max_source_gap_ms"] = 200.0;
  EXPECT_FALSE(doctor_v2::status("owner", "app", evidence).at("stable").get<bool>());
}

TEST(DoctorV2CadenceTests, SourceTimeCannotAuthorizeAFixedRatePacingTrial) {
  doctor_v2_flag_t enabled(true);
  covered_samples(90.0, 60.0);
  const auto status = doctor_v2::status("owner", "app", source_time_evidence());
  EXPECT_FALSE(status.at("actions_exposed").get<bool>());
  EXPECT_FALSE(status.at("trial_metrics").at("ready").get<bool>());
}

TEST(DoctorV2CadenceTests, PrivateSourcePacingWaitAndStaleEvidenceNeverBecomeHealthyFixedGrid) {
  doctor_v2_flag_t enabled;
  covered_samples(120.0, 120.0);
  for (const char *reason : {"source_time_unproven", "pacing_evidence_stale"}) {
    auto evidence = host_evidence(120.0);
    evidence["vrr"] = source_time_evidence()["vrr"];
    evidence["vrr"].update({{"effective", false}, {"pacing_mode", "source_time_pending"}, {"fallback_reason", reason}});
    const auto status = doctor_v2::status("owner", "app", evidence);
    EXPECT_EQ(status["state"], "collecting") << reason;
    EXPECT_FALSE(status["stable"].get<bool>()) << reason;
    EXPECT_FALSE(status["coverage_adequate"].get<bool>()) << reason;
  }
}

TEST(DoctorV2CadenceTests, PrivateUnsupportedAndRevokedPathsRetainHonestLegacyPolicy) {
  doctor_v2_flag_t enabled;
  covered_samples(120.0, 120.0);
  for (const char *reason : {"unsupported_codec", "unsupported_loop", "authority_lost"}) {
    auto evidence = host_evidence(120.0);
    evidence["vrr"] = source_time_evidence()["vrr"];
    evidence["vrr"].update({{"effective", false}, {"pacing_mode", "fixed_grid"}, {"fallback_reason", reason}});
    EXPECT_TRUE(doctor_v2::status("owner", "app", evidence)["coverage_adequate"].get<bool>()) << reason;
    evidence["vrr"]["requested"] = "false";
    EXPECT_FALSE(doctor_v2::status("owner", "app", evidence)["coverage_adequate"].get<bool>()) << reason;
  }
}
