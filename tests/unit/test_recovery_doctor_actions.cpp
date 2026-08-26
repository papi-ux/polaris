/**
 * @file tests/unit/test_recovery_doctor_actions.cpp
 * @brief Trusted Doctor execution tests for next-launch recovery profiles.
 */
#include "../tests_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

#include <src/doctor_actions.h>
#include <src/recovery_profile.h>

namespace {
  std::atomic_uint64_t recovery_action_counter {0};

  class RecoveryDoctorActionTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() /
        ("polaris-recovery-doctor-" + std::to_string(nonce) + "-" +
         std::to_string(recovery_action_counter.fetch_add(1)));
      std::filesystem::create_directories(directory);

      context.active_owner = true;
      context.host_tuning_allowed = true;
      context.owner_uuid = "client-a";
      context.device_name = "RetroidPocket6";
      context.app_uuid = "game-a";
      context.app_name = "Control Ultimate Edition";
      context.launch_instance_id = "launch-a";
      context.session_generation = 1;
      context.effective_stream_display_mode = "host_virtual_display";
      context.state_path = directory / "recovery_profiles.json";
      context.stats.streaming = true;
      context.stats.width = 1920;
      context.stats.height = 1080;
      context.stats.encode_target_fps = 60;
      context.stats.bitrate_kbps = 30000;
      context.stats.codec = "AV1";
      context.stats.stream_hdr_enabled = true;
      context.health = {
        {"relaunch_recommended", true},
        {"safe_display_mode", "headless"},
        {"safe_target_fps", 40},
        {"safe_bitrate_kbps", 18000},
        {"safe_codec", "hevc"},
        {"safe_hdr", false},
        {"doctor", {
          {"result_id", source_result_id},
          {"safe_recovery_action", {
            {"id", "apply_recovery_profile_next_launch"},
            {"kind", "next_launch_profile"},
            {"requires_confirmation", true},
            {"requires_owner", true},
            {"owner_tuning_allowed", true},
            {"payload_preview", {
              {"source_result_id", source_result_id},
              {"app_uuid", "game-a"}
            }},
            {"undo", {{"supported", true}}}
          }}
        }}
      };
    }

    void TearDown() override {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }

    nlohmann::json apply_request() const {
      return {
        {"action_id", "apply_recovery_profile_next_launch"},
        {"source_result_id", source_result_id},
        {"app_uuid", "game-a"},
        {"confirmed", true},
        // All of these are attacker-controlled and must be ignored.
        {"display_mode", "desktop_display"},
        {"target_fps", 240},
        {"target_bitrate_kbps", 300000},
        {"preferred_codec", "arbitrary"},
        {"hdr", true}
      };
    }

    std::filesystem::path directory;
    doctor_actions::recovery_action_context_t context;
    const std::string source_result_id = "doctor-v2-watch-frame_pacing-none";
  };
}

TEST_F(RecoveryDoctorActionTest, RequiresConfirmationCurrentEvidenceAndOwnerTuning) {
  auto unconfirmed = apply_request();
  unconfirmed["confirmed"] = false;
  EXPECT_EQ(
    doctor_actions::execute(unconfirmed, context).value("state", ""),
    "confirmation_required"
  );

  auto stale = apply_request();
  stale["source_result_id"] = "doctor-v2-stale";
  EXPECT_EQ(doctor_actions::execute(stale, context).value("state", ""), "evidence_changed");

  auto viewer = context;
  viewer.active_owner = false;
  EXPECT_EQ(doctor_actions::execute(apply_request(), viewer).value("state", ""), "evidence_changed");
}

TEST_F(RecoveryDoctorActionTest, RejectsConfirmationReplayedForAnotherGame) {
  auto game_b = context;
  game_b.app_uuid = "game-b";
  game_b.app_name = "Another Game";

  EXPECT_EQ(
    doctor_actions::execute(apply_request(), game_b).value("state", ""),
    "evidence_changed"
  );

  auto forged_for_b = apply_request();
  forged_for_b["app_uuid"] = "game-b";
  EXPECT_EQ(
    doctor_actions::execute(forged_for_b, game_b).value("state", ""),
    "evidence_changed"
  );
  EXPECT_FALSE(recovery_profile::queued(
    context.state_path, context.owner_uuid, "game-b"
  ));
}

TEST_F(RecoveryDoctorActionTest, QueuesOnlyHostDerivedProfileAndIsIdempotent) {
  const auto first = doctor_actions::execute(apply_request(), context);
  ASSERT_TRUE(first.value("status", false));
  EXPECT_TRUE(first.value("changed", false));
  EXPECT_EQ(first.value("recovery_state", ""), "queued");
  const auto profile = first.at("safe_profile");
  EXPECT_EQ(profile.value("stream_display_mode", ""), "headless_stream");
  EXPECT_EQ(profile.value("width", 0), 1920);
  EXPECT_EQ(profile.value("height", 0), 1080);
  EXPECT_EQ(profile.value("target_fps", 0), 40);
  EXPECT_EQ(profile.value("target_bitrate_kbps", 0), 18000);
  EXPECT_EQ(profile.value("preferred_codec", ""), "hevc");
  EXPECT_FALSE(profile.value("hdr", true));
  EXPECT_TRUE(profile.value("requires_fresh_launch", false));

  const auto repeated = doctor_actions::execute(apply_request(), context);
  EXPECT_EQ(repeated.value("run_id", ""), first.value("run_id", ""));
  EXPECT_TRUE(repeated.value("idempotent", false));
  EXPECT_FALSE(repeated.value("changed", true));
}

TEST_F(RecoveryDoctorActionTest, VerifiesTrustedEffectiveSettingsAfterConnectAndConsumesOnce) {
  const auto queued = doctor_actions::execute(apply_request(), context);
  const auto run_id = queued.value("run_id", "");
  ASSERT_FALSE(run_id.empty());

  const auto rejected = doctor_actions::execute({
    {"action_id", "verify_recovery_profile_next_launch"},
    {"run_id", run_id},
    {"target_fps", 40}
  }, context);
  EXPECT_EQ(rejected.value("state", ""), "rejected");
  EXPECT_EQ(rejected.value("recovery_state", ""), "queued");

  context.stats.encode_target_fps = 40;
  context.stats.bitrate_kbps = 18000;
  context.stats.codec = "HEVC";
  context.stats.stream_hdr_enabled = false;
  context.effective_stream_display_mode = "headless_stream";
  const auto same_launch = doctor_actions::execute({
    {"action_id", "verify_recovery_profile_next_launch"},
    {"run_id", run_id}
  }, context);
  EXPECT_FALSE(same_launch.value("status", true));
  EXPECT_EQ(same_launch.value("recovery_state", ""), "queued");
  EXPECT_NE(
    std::find(same_launch.at("mismatches").begin(), same_launch.at("mismatches").end(), "fresh_launch"),
    same_launch.at("mismatches").end()
  );

  context.launch_instance_id = "launch-b";
  context.session_generation = 2;
  const auto applied = doctor_actions::execute({
    {"action_id", "verify_recovery_profile_next_launch"},
    {"run_id", run_id}
  }, context);
  EXPECT_TRUE(applied.value("status", false));
  EXPECT_TRUE(applied.value("changed", false));
  EXPECT_EQ(applied.value("recovery_state", ""), "applied");

  const auto repeated = doctor_actions::execute({
    {"action_id", "verify_recovery_profile_next_launch"},
    {"run_id", run_id}
  }, context);
  EXPECT_TRUE(repeated.value("status", false));
  EXPECT_TRUE(repeated.value("idempotent", false));
  EXPECT_FALSE(repeated.value("changed", true));
}

TEST_F(RecoveryDoctorActionTest, UndoCancelsOnlyTheMatchingQueuedRun) {
  const auto queued = doctor_actions::execute(apply_request(), context);
  const auto run_id = queued.value("run_id", "");

  const auto wrong = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", "recovery-run-imposter"}
  }, context);
  EXPECT_FALSE(wrong.value("status", true));
  EXPECT_TRUE(recovery_profile::queued(
    context.state_path, context.owner_uuid, context.app_uuid
  ));

  auto other_owner = context;
  other_owner.active_owner = false;
  other_owner.caller_is_viewer = true;
  other_owner.owner_uuid = "client-b";
  const auto cross_owner = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", run_id}
  }, other_owner);
  EXPECT_FALSE(cross_owner.value("status", true));
  EXPECT_TRUE(recovery_profile::queued(
    context.state_path, context.owner_uuid, context.app_uuid
  ));

  // The queued owner may be disconnected while another paired client owns the
  // live stream. Exact owner scoping still lets A cancel only A's record.
  context.active_owner = false;
  context.caller_is_viewer = true;
  context.stats.streaming = true;
  context.host_tuning_allowed = true;
  context.app_uuid = "game-b";
  const auto undone = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", run_id}
  }, context);
  EXPECT_TRUE(undone.value("status", false));
  EXPECT_EQ(undone.value("recovery_state", ""), "undone");
  EXPECT_FALSE(recovery_profile::queued(
    context.state_path, context.owner_uuid, "game-a"
  ));
}

TEST(PairedDoctorRoutePolicyTests, AllowsDisconnectedOwnerUndoButRejectsViewerAndOtherActions) {
  EXPECT_TRUE(doctor_actions::paired_route_allowed("undo", false, false));
  EXPECT_TRUE(doctor_actions::paired_route_allowed("undo", true, true));
  EXPECT_TRUE(doctor_actions::paired_route_allowed("undo", true, false));
  EXPECT_FALSE(doctor_actions::paired_route_allowed(
    "apply_recovery_profile_next_launch", false, false
  ));
  EXPECT_TRUE(doctor_actions::paired_route_allowed(
    "apply_recovery_profile_next_launch", true, true
  ));
}
