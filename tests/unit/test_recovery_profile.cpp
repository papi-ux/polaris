/**
 * @file tests/unit/test_recovery_profile.cpp
 * @brief Durable next-launch recovery profile contract tests.
 */
#include "../tests_common.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <utility>

#if defined(__linux__)
  #include <sys/stat.h>
#endif

#include <src/recovery_profile.h>

namespace {
  std::atomic_uint64_t path_counter {0};

  recovery_profile::safe_profile_t safe_profile() {
    return {
      .stream_display_mode = "headless_stream",
      .width = 1920,
      .height = 1080,
      .target_fps = 40,
      .target_bitrate_kbps = 18000,
      .preferred_codec = "hevc",
      .hdr = false,
    };
  }

  recovery_profile::optimizer_constraints_t optimizer_constraints(
    int width = 1920,
    int height = 1080
  ) {
    return {
      .paired_width = width,
      .paired_height = height,
      .supported_codecs = {"h264", "hevc", "av1"},
      .hdr_supported = true,
      .allowed_stream_display_modes = {"headless_stream", "host_virtual_display"},
    };
  }

  nlohmann::json queue_profile(const std::filesystem::path &target,
                               const std::string &owner,
                               const std::string &app,
                               const std::string &result_id,
                               const recovery_profile::safe_profile_t &profile,
                               std::int64_t now,
                               const std::string &launch_instance_id = "launch-a",
                               std::uint64_t session_generation = 1) {
    return recovery_profile::queue(
      target,
      owner,
      app,
      result_id,
      profile,
      launch_instance_id,
      session_generation,
      now
    );
  }

  recovery_profile::observed_launch_t matching_launch(std::string owner = "client-a",
                                                        std::string app = "game-a") {
    return {
      .streaming = true,
      .owner_uuid = std::move(owner),
      .app_uuid = std::move(app),
      .stream_display_mode = "headless_stream",
      .width = 1920,
      .height = 1080,
      .target_fps = 40,
      .bitrate_kbps = 18000,
      .codec = "hevc",
      .hdr = false,
      .launch_instance_id = "launch-b",
      .session_generation = 2,
    };
  }

  class RecoveryProfileTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() /
                  ("polaris-recovery-profile-" + std::to_string(nonce) + "-" +
                   std::to_string(path_counter.fetch_add(1)));
      std::filesystem::create_directories(directory);
      target = directory / "recovery_profiles.json";
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

TEST_F(RecoveryProfileTest, QueuesPrivateDurableProfileAndRepeatsIdempotently) {
  const auto first = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  ASSERT_TRUE(first.value("status", false));
  EXPECT_TRUE(first.value("changed", false));
  EXPECT_EQ(first.value("state", ""), "queued");
  EXPECT_EQ(first.value("expires_at", 0LL), now + recovery_profile::lifetime_seconds);
  const auto run_id = first.value("run_id", "");
  ASSERT_FALSE(run_id.empty());

  const auto repeated = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now + 5
  );
  EXPECT_TRUE(repeated.value("status", false));
  EXPECT_FALSE(repeated.value("changed", true));
  EXPECT_TRUE(repeated.value("idempotent", false));
  EXPECT_EQ(repeated.value("run_id", ""), run_id);

#if defined(__linux__)
  struct stat metadata {};
  ASSERT_EQ(::stat(target.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
#endif

  // A fresh load through the public API proves the record does not depend on
  // process memory and therefore survives a Polaris restart.
  const auto resumed = recovery_profile::status(target, "client-a", "game-a", now + 10);
  EXPECT_EQ(resumed.value("run_id", ""), run_id);
  EXPECT_EQ(resumed.value("recovery_state", ""), "queued");
}

TEST_F(RecoveryProfileTest, IsolatesOwnerAndCanonicalGame) {
  ASSERT_TRUE(queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  ).value("status", false));

  EXPECT_EQ(recovery_profile::status(target, "client-b", "game-a", now).value("state", ""), "none");
  EXPECT_EQ(recovery_profile::status(target, "client-a", "game-b", now).value("state", ""), "none");
  EXPECT_FALSE(recovery_profile::queued(target, "client-b", "game-a", now));
  EXPECT_FALSE(recovery_profile::queued(target, "client-a", "game-b", now));
}

TEST_F(RecoveryProfileTest, PreviewDoesNotConsumeAndUndoOnlyRemovesExactQueuedRun) {
  const auto queued = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  const auto run_id = queued.value("run_id", "");
  ASSERT_TRUE(recovery_profile::queued(target, "client-a", "game-a", now + 1));
  ASSERT_TRUE(recovery_profile::queued(target, "client-a", "game-a", now + 2));

  const auto wrong_owner = recovery_profile::undo(target, "client-b", "game-a", run_id, now + 3);
  EXPECT_FALSE(wrong_owner.value("status", true));
  EXPECT_TRUE(recovery_profile::queued(target, "client-a", "game-a", now + 4));

  const auto undone = recovery_profile::undo(target, "client-a", "game-a", run_id, now + 5);
  EXPECT_TRUE(undone.value("status", false));
  EXPECT_EQ(undone.value("recovery_state", ""), "undone");
  EXPECT_FALSE(recovery_profile::queued(target, "client-a", "game-a", now + 6));
  EXPECT_EQ(recovery_profile::status(target, "client-a", "game-a", now + 6).value("state", ""), "none");
}

TEST_F(RecoveryProfileTest, OptimizerOverlayPreservesExistingFieldsAndPairedResolution) {
  ASSERT_TRUE(queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  ).value("status", false));
  const auto record = recovery_profile::prepare_for_optimizer(
    target, "client-a", "game-a", optimizer_constraints(2560, 1440), now + 1
  );
  ASSERT_TRUE(record);

  const auto optimized = recovery_profile::overlay_optimization(
    {{"nvenc_tune", "p4"}, {"normalization_reason", "paired profile applied"}},
    *record
  );
  EXPECT_EQ(optimized.value("nvenc_tune", ""), "p4");
  EXPECT_EQ(optimized.value("normalization_reason", ""), "paired profile applied");
  EXPECT_EQ(optimized.value("display_mode", ""), "2560x1440x40");
  EXPECT_EQ(optimized.value("target_bitrate_kbps", 0), 18000);
  EXPECT_EQ(optimized.value("preferred_codec", ""), "hevc");
  EXPECT_FALSE(optimized.value("hdr", true));
  EXPECT_EQ(optimized.value("stream_display_mode", ""), "headless_stream");
  EXPECT_TRUE(optimized.value("requires_fresh_launch", false));
  EXPECT_EQ(optimized.value("recovery_run_id", ""), record->run_id);
  EXPECT_EQ(optimized.value("recovery_state", ""), "queued");
  EXPECT_EQ(optimized.at("recovery_profile").value("width", 0), 2560);
  EXPECT_EQ(optimized.at("recovery_profile").value("height", 0), 1440);
  EXPECT_TRUE(optimized.at("stability").value("relaunch_required", false));
  EXPECT_EQ(optimized.at("recovery_policy").value("state", ""), "recovery_queued");
  EXPECT_TRUE(recovery_profile::queued(target, "client-a", "game-a", now + 2));
}

TEST_F(RecoveryProfileTest, OptimizerPersistsPairedResolutionForTrustedVerification) {
  const auto queued = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  const auto run_id = queued.value("run_id", "");

  const auto prepared = recovery_profile::prepare_for_optimizer(
    target, "client-a", "game-a", optimizer_constraints(2560, 1440), now + 1
  );
  ASSERT_TRUE(prepared);
  EXPECT_EQ(prepared->profile.width, 2560);
  EXPECT_EQ(prepared->profile.height, 1440);
  auto observed = matching_launch();
  observed.width = 2560;
  observed.height = 1440;
  EXPECT_TRUE(recovery_profile::verify(
    target, "client-a", "game-a", run_id, observed, now + 2
  ).value("status", false));
}

TEST_F(RecoveryProfileTest, RepeatApplyAfterOptimizerPreparationKeepsRunAndPreparedProfile) {
  const auto first = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  const auto run_id = first.value("run_id", "");
  ASSERT_FALSE(run_id.empty());
  ASSERT_TRUE(recovery_profile::prepare_for_optimizer(
    target, "client-a", "game-a", optimizer_constraints(2560, 1440), now + 1
  ));

  const auto repeated = queue_profile(
    target,
    "client-a",
    "game-a",
    "doctor-v2-watch-frame_pacing-none",
    safe_profile(),
    now + 2
  );
  EXPECT_TRUE(repeated.value("idempotent", false));
  EXPECT_EQ(repeated.value("run_id", ""), run_id);
  EXPECT_EQ(repeated.at("safe_profile").value("width", 0), 2560);
  EXPECT_EQ(repeated.at("safe_profile").value("height", 0), 1440);

  const auto stored = recovery_profile::queued(target, "client-a", "game-a", now + 3);
  ASSERT_TRUE(stored);
  EXPECT_TRUE(stored->optimizer_prepared);
  EXPECT_EQ(stored->profile.width, 2560);
  EXPECT_EQ(stored->profile.height, 1440);
}

TEST_F(RecoveryProfileTest, RevalidatesQueuedCodecHdrAndTopologyAgainstCurrentCapabilities) {
  auto originally_safe = safe_profile();
  originally_safe.stream_display_mode = "host_virtual_display";
  originally_safe.preferred_codec = "av1";
  originally_safe.hdr = true;
  const auto queued = queue_profile(
    target,
    "client-a",
    "game-a",
    "doctor-v2-watch-frame_pacing-none",
    originally_safe,
    now
  );
  const auto run_id = queued.value("run_id", "");

  auto drifted = optimizer_constraints();
  drifted.supported_codecs = {"h264"};
  drifted.hdr_supported = false;
  drifted.allowed_stream_display_modes = {"headless_stream"};
  const auto prepared = recovery_profile::prepare_for_optimizer(
    target, "client-a", "game-a", drifted, now + 1
  );
  ASSERT_TRUE(prepared);
  EXPECT_EQ(prepared->run_id, run_id);
  EXPECT_EQ(prepared->profile.preferred_codec, "h264");
  EXPECT_FALSE(prepared->profile.hdr);
  EXPECT_EQ(prepared->profile.stream_display_mode, "headless_stream");

  const auto optimized = recovery_profile::overlay_optimization(
    {{"preferred_codec", "hevc"}, {"hdr", true}, {"stream_display_mode", "host_virtual_display"}},
    *prepared
  );
  EXPECT_EQ(optimized.value("preferred_codec", ""), "h264");
  EXPECT_FALSE(optimized.value("hdr", true));
  EXPECT_EQ(optimized.value("stream_display_mode", ""), "headless_stream");
}

TEST_F(RecoveryProfileTest, OwnerStatusReconstructsQueueWithoutAnActiveGame) {
  ASSERT_TRUE(queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  ).value("status", false));
  ASSERT_TRUE(queue_profile(
    target, "client-b", "game-b", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  ).value("status", false));

  const auto owner_records = recovery_profile::statuses_for_owner(target, "client-a", now + 1);
  ASSERT_EQ(owner_records.size(), 1);
  EXPECT_EQ(owner_records.front().value("app_uuid", ""), "game-a");
  EXPECT_EQ(owner_records.front().value("recovery_state", ""), "queued");
}

TEST_F(RecoveryProfileTest, MismatchedLaunchStaysQueuedAndMatchingLaunchConsumesExactlyOnce) {
  const auto queued = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  const auto run_id = queued.value("run_id", "");

  auto wrong = matching_launch();
  wrong.target_fps = 60;
  const auto rejected = recovery_profile::verify(
    target, "client-a", "game-a", run_id, wrong, now + 1
  );
  EXPECT_FALSE(rejected.value("status", true));
  EXPECT_EQ(rejected.value("state", ""), "rejected");
  EXPECT_EQ(rejected.value("recovery_state", ""), "queued");
  EXPECT_TRUE(recovery_profile::queued(target, "client-a", "game-a", now + 2));

  const auto applied = recovery_profile::verify(
    target, "client-a", "game-a", run_id, matching_launch(), now + 3
  );
  EXPECT_TRUE(applied.value("status", false));
  EXPECT_TRUE(applied.value("changed", false));
  EXPECT_EQ(applied.value("recovery_state", ""), "applied");
  EXPECT_FALSE(recovery_profile::queued(target, "client-a", "game-a", now + 4));

  const auto repeated = recovery_profile::verify(
    target, "client-a", "game-a", run_id, matching_launch(), now + 5
  );
  EXPECT_TRUE(repeated.value("status", false));
  EXPECT_FALSE(repeated.value("changed", true));
  EXPECT_TRUE(repeated.value("idempotent", false));
  EXPECT_EQ(repeated.value("recovery_state", ""), "applied");
}

TEST_F(RecoveryProfileTest, MatchingSettingsOnTheQueuedLaunchCannotConsume) {
  const auto queued = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  auto same_launch = matching_launch();
  same_launch.launch_instance_id = "launch-a";
  same_launch.session_generation = 1;
  const auto rejected = recovery_profile::verify(
    target,
    "client-a",
    "game-a",
    queued.value("run_id", ""),
    same_launch,
    now + 1
  );
  EXPECT_FALSE(rejected.value("status", true));
  EXPECT_EQ(rejected.value("recovery_state", ""), "queued");
  EXPECT_TRUE(recovery_profile::queued(target, "client-a", "game-a", now + 2));
}

TEST(RecoveryProfileCanonicalAppTests, RejectsAmbiguousNamesAndPrefersExactIdentifiers) {
  const std::vector<recovery_profile::app_identity_t> apps {
    {.uuid = "uuid-a", .id = "101", .name = "Control"},
    {.uuid = "uuid-b", .id = "202", .name = "Control"},
    {.uuid = "uuid-c", .id = "303", .name = "Another Game"},
  };

  EXPECT_FALSE(recovery_profile::resolve_canonical_app_uuid("Control", apps));
  EXPECT_EQ(recovery_profile::resolve_canonical_app_uuid("UUID-B", apps), "uuid-b");
  EXPECT_EQ(recovery_profile::resolve_canonical_app_uuid("303", apps), "uuid-c");
  EXPECT_EQ(recovery_profile::resolve_canonical_app_uuid("another game", apps), "uuid-c");
}

TEST_F(RecoveryProfileTest, ExpiresAtTwentyFourHoursAndCannotBeConsumed) {
  const auto queued = queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  );
  const auto run_id = queued.value("run_id", "");
  const auto expiry = now + recovery_profile::lifetime_seconds;

  const auto expired = recovery_profile::status(target, "client-a", "game-a", expiry);
  EXPECT_EQ(expired.value("recovery_state", ""), "expired");
  EXPECT_FALSE(expired.at("undo").value("available", true));
  EXPECT_FALSE(recovery_profile::queued(target, "client-a", "game-a", expiry));

  const auto verify = recovery_profile::verify(
    target, "client-a", "game-a", run_id, matching_launch(), expiry + 1
  );
  EXPECT_FALSE(verify.value("status", true));
  EXPECT_EQ(verify.value("recovery_state", ""), "expired");
}

TEST_F(RecoveryProfileTest, RejectsInvalidHostProfilesAndInsecureState) {
  auto invalid = safe_profile();
  invalid.preferred_codec = "arbitrary-client-codec";
  EXPECT_FALSE(queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", invalid, now
  ).value("status", true));

  ASSERT_TRUE(queue_profile(
    target, "client-a", "game-a", "doctor-v2-watch-frame_pacing-none", safe_profile(), now
  ).value("status", false));
  std::filesystem::permissions(target, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
  const auto rejected = recovery_profile::status(target, "client-a", "game-a", now + 1);
  EXPECT_FALSE(rejected.value("status", true));
  EXPECT_EQ(rejected.value("state", ""), "rejected");
}
