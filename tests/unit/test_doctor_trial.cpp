#include "../tests_common.h"

#include <src/doctor_trial.h>

#include <atomic>
#include <chrono>
#include <filesystem>

#if defined(__linux__)
  #include <sys/stat.h>
#endif

namespace {
  std::atomic_uint64_t path_counter {0};

  doctor_trial::effective_settings_t settings(int target_fps = 120) {
    return {
      .topology = "desktop_display",
      .width = 1920,
      .height = 1080,
      .target_fps = target_fps,
      .bitrate_kbps = 40000,
      .codec = "hevc",
      .hdr = false,
    };
  }

  nlohmann::json evidence(std::uint64_t generation = 10,
                          double target_fps = 120.0,
                          double rendered_fps = 58.0,
                          double loss = 0.1,
                          double rtt = 10.0,
                          double decode = 3.0,
                          std::optional<double> host = 5.0) {
    nlohmann::json metrics {
      {"ready", true}, {"duration_seconds", 60}, {"coverage", 0.95},
      {"session_generation", generation}, {"target_fps", target_fps},
      {"rendered_fps", rendered_fps},
      {"pacing_error_pct", 100.0 * std::max(0.0, target_fps - rendered_fps) / target_fps},
      {"confirmed_media_loss_pct", loss}, {"rtt_ms", rtt},
      {"decode_latency_ms", decode},
      {"host_processing_latency_ms", host ? nlohmann::json(*host) : nlohmann::json(nullptr)}
    };
    return {{"state", "classified"}, {"trial_metrics", std::move(metrics)}};
  }

  class DoctorTrialTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() /
        ("polaris-doctor-trial-" + std::to_string(nonce) + "-" +
         std::to_string(path_counter.fetch_add(1)));
      std::filesystem::create_directories(directory);
      target = directory / "doctor_trials.json";
    }

    void TearDown() override {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path directory;
    std::filesystem::path target;
    static constexpr std::int64_t now = 1'800'000'000;
  };
}

TEST_F(DoctorTrialTest, ProposesOnlyOneHostDerivedDimensionAndPersistsPrivately) {
  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(), settings(), now
  );

  ASSERT_TRUE(proposal.value("status", false));
  EXPECT_EQ(proposal.value("state", ""), "proposed");
  EXPECT_EQ(proposal.value("changed_dimension", ""), "stream_fps_ceiling");
  EXPECT_EQ(proposal.at("candidate").value("target_fps", 0), 60);
  EXPECT_EQ(proposal.at("preserved").value("topology", ""), "desktop_display");
  EXPECT_EQ(proposal.at("preserved").value("width", 0), 1920);
  EXPECT_EQ(proposal.at("preserved").value("bitrate_kbps", 0), 40000);
  EXPECT_FALSE(proposal.value("becomes_policy_automatically", true));

#if defined(__linux__)
  struct stat metadata {};
  ASSERT_EQ(::stat(target.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
#endif
}

TEST_F(DoctorTrialTest, RequiresExplicitConfirmationAndFreshLaunchInstance) {
  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(), settings(), now
  );
  const auto run_id = proposal.value("run_id", "");
  ASSERT_FALSE(run_id.empty());
  ASSERT_EQ(doctor_trial::confirm(
    target, "owner-a", "app-a", run_id, now + 1
  ).value("state", ""), "queued");

  EXPECT_FALSE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "baseline-launch", now + 2
  ));
  const auto trial = doctor_trial::begin_launch(
    target, "owner-a", "app-a", "trial-launch", now + 3
  );

  ASSERT_TRUE(trial);
  EXPECT_EQ(trial->run_id, run_id);
  EXPECT_EQ(trial->target_fps, 60);
  EXPECT_EQ(doctor_trial::status(target, "owner-a", "app-a", now + 4).value("state", ""), "running");
  EXPECT_FALSE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "another-launch", now + 5
  ));
}

TEST_F(DoctorTrialTest, TrialBindsTheFullCandidateWindowHostGeneration) {
  const auto mismatched = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(9), settings(), now
  );
  EXPECT_FALSE(mismatched.value("status", true));

  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(10), settings(), now
  );
  const auto run_id = proposal.value("run_id", "");
  ASSERT_TRUE(doctor_trial::confirm(target, "owner-a", "app-a", run_id, now + 1).value("status", false));
  ASSERT_TRUE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "trial-launch", now + 2
  ));

  const auto later_launch_window = doctor_trial::observe(
    target, "owner-a", "app-a", "later-launch",
    evidence(11, 60.0, 59.0), settings(60), false, now + 88
  );
  EXPECT_EQ(later_launch_window.value("state", ""), "running");
  EXPECT_EQ(
    later_launch_window.value("reason_code", ""),
    "trial_launch_instance_mismatch"
  );

  const auto stale_baseline_window = doctor_trial::observe(
    target, "owner-a", "app-a", "trial-launch",
    evidence(10, 120.0, 58.0), settings(60), false, now + 89
  );
  EXPECT_EQ(stale_baseline_window.value("state", ""), "collecting");
  EXPECT_EQ(stale_baseline_window.value("reason_code", ""), "trial_target_window_mismatch");

  const auto result = doctor_trial::observe(
    target, "owner-a", "app-a", "trial-launch",
    evidence(12, 60.0, 59.0), settings(60), false, now + 90
  );
  EXPECT_EQ(result.value("state", ""), "improved");
  EXPECT_EQ(result.at("result").value("session_generation", 0), 12);
}

TEST_F(DoctorTrialTest, CrashAlwaysMarksTheRunningTrialWorse) {
  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(), settings(), now
  );
  const auto run_id = proposal.value("run_id", "");
  ASSERT_TRUE(doctor_trial::confirm(target, "owner-a", "app-a", run_id, now + 1).value("status", false));
  ASSERT_TRUE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "trial-launch", now + 2
  ));

  const auto result = doctor_trial::observe(
    target, "owner-a", "app-a", "trial-launch",
    nlohmann::json::object(), settings(60), true, now + 3
  );
  EXPECT_EQ(result.value("state", ""), "worse");
  EXPECT_EQ(result.value("reason_code", ""), "trial_session_crashed");
}

TEST_F(DoctorTrialTest, MarksImprovedOnlyAfterFullWindowWithoutGuardrailRegression) {
  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(), settings(), now
  );
  const auto run_id = proposal.value("run_id", "");
  ASSERT_TRUE(doctor_trial::confirm(target, "owner-a", "app-a", run_id, now + 1).value("status", false));
  ASSERT_TRUE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "trial-launch", now + 2
  ));

  const auto result = doctor_trial::observe(
    target,
    "owner-a",
    "app-a",
    "trial-launch",
    evidence(11, 60.0, 59.0, 0.1, 10.5, 3.1, 5.2),
    settings(60),
    false,
    now + 90
  );

  EXPECT_EQ(result.value("state", ""), "improved");
  EXPECT_GE(result.value("pacing_improvement_ratio", 0.0), 0.20);
  EXPECT_FALSE(result.value("guardrail_regressed", true));
}

TEST_F(DoctorTrialTest, ConfoundedSettingsAreInconclusiveAndNeverBecomePolicy) {
  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(), settings(), now
  );
  const auto run_id = proposal.value("run_id", "");
  ASSERT_TRUE(doctor_trial::confirm(target, "owner-a", "app-a", run_id, now + 1).value("status", false));
  ASSERT_TRUE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "trial-launch", now + 2
  ));
  auto confounded = settings(60);
  confounded.bitrate_kbps = 20000;

  const auto result = doctor_trial::observe(
    target, "owner-a", "app-a", "trial-launch",
    evidence(11, 60.0, 59.0), confounded, false, now + 90
  );

  EXPECT_EQ(result.value("state", ""), "inconclusive");
  EXPECT_EQ(result.value("reason_code", ""), "confounded_trial_settings");
  EXPECT_FALSE(result.value("becomes_policy_automatically", true));
}

TEST_F(DoctorTrialTest, CanCancelQueuedRunWithoutAConnectedSession) {
  const auto proposal = doctor_trial::propose(
    target, "owner-a", "app-a", "baseline-launch", 10, evidence(), settings(), now
  );
  const auto run_id = proposal.value("run_id", "");
  ASSERT_TRUE(doctor_trial::confirm(target, "owner-a", "app-a", run_id, now + 1).value("status", false));

  const auto cancelled = doctor_trial::cancel(
    target, "owner-a", "app-a", run_id, now + 2
  );

  EXPECT_EQ(cancelled.value("state", ""), "cancelled");
  EXPECT_FALSE(cancelled.value("cancellable", true));
  EXPECT_FALSE(doctor_trial::begin_launch(
    target, "owner-a", "app-a", "trial-launch", now + 3
  ));
}
