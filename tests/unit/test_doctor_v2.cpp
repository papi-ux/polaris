#include "../tests_common.h"

#include <src/doctor_v2.h>

#include <cstdlib>
#include <optional>
#include <string>

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
