#include "src/live_tuning.h"
#include "src/configuration_store.h"
#include "src/config.h"
#include "src/private_state_file.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <future>
#include <fstream>

namespace {
  class LiveTuningTest: public testing::Test {
   protected:
    std::string old_path;
    decltype(config::video.adaptive_bitrate) old_config;
    std::filesystem::path directory;
    void SetUp() override {
      old_path = config::sunshine.config_file;
      old_config = config::video.adaptive_bitrate;
      directory = std::filesystem::temp_directory_path() /
        ("polaris-tuning-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directory(directory);
      config::sunshine.config_file = (directory / "polaris.conf").string();
      ASSERT_TRUE(private_state_file::write_atomic(config::sunshine.config_file,
        "# Preserve unrelated preferences\nai_enabled = disabled\nadaptive_bitrate_enabled = enabled\n"));
      config::video.adaptive_bitrate.enabled = true;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
    }
    void TearDown() override {
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
      config::sunshine.config_file = old_path;
      config::video.adaptive_bitrate = old_config;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
      adaptive_bitrate::set_session_scope(0, "", false);
      std::filesystem::remove_all(directory);
    }
  };
}

TEST_F(LiveTuningTest, ReportsSavedPreferenceBeforeFirstStreamWithoutAi) {
  const auto value = live_tuning::snapshot({});
  EXPECT_EQ(value["enabled"], true);
  EXPECT_EQ(value["state"], "waiting");
  EXPECT_EQ(value["applied_bitrate_kbps"], 0);
}

TEST_F(LiveTuningTest, SaveIsRevisionConditionalAndRetryDoesNotChangeAuthority) {
  auto authority = doctor_actions::acquire_admin_global_control();
  const auto revision = configuration_store::revision(config::sunshine.config_file, true);
  ASSERT_TRUE(live_tuning::set_enabled(authority, false, revision)["status"].get<bool>());
  const auto saved = private_state_file::read_secure(config::sunshine.config_file, 4096).payload;
  EXPECT_NE(saved.find("# Preserve unrelated preferences"), std::string::npos);
  EXPECT_NE(saved.find("ai_enabled = disabled"), std::string::npos);
  EXPECT_EQ(live_tuning::set_enabled(authority, true, revision)["http_status"], 412);
  const auto current = adaptive_bitrate::get_doctor_state();
  ASSERT_TRUE(live_tuning::set_enabled(authority, false)["status"].get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, current.revision);
}

TEST_F(LiveTuningTest, FailedCommitDoesNotApplyPreference) {
  auto authority = doctor_actions::acquire_admin_global_control();
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename);
  EXPECT_EQ(live_tuning::set_enabled(authority, false)["http_status"], 500);
  EXPECT_TRUE(adaptive_bitrate::get_state().configured_enabled);
}

TEST_F(LiveTuningTest, ReleasedAndMovedFromGuardsCannotSave) {
  auto released = doctor_actions::acquire_admin_global_control();
  released.release();
  EXPECT_FALSE(static_cast<bool>(released));
  EXPECT_EQ(live_tuning::set_enabled(released, false)["http_status"], 403);
  auto original = doctor_actions::acquire_admin_global_control();
  auto moved = std::move(original);
  EXPECT_FALSE(static_cast<bool>(original));
  EXPECT_EQ(live_tuning::set_enabled(original, false)["http_status"], 403);
  EXPECT_TRUE(adaptive_bitrate::get_state().configured_enabled);
}

TEST_F(LiveTuningTest, NeverAttributesAnotherSessionOrSharedEncoderBitrate) {
  adaptive_bitrate::state_t c;
  c.session_exclusive = true;
  c.session_generation = 2;
  c.app_session_id = "new";
  c.runtime_update_supported = true;
  c.applied_bitrate_kbps = 14000;
  c.target_bitrate_kbps = 20000;
  stream_stats::stats_t s;
  s.streaming = true;
  s.session_generation = 1;
  s.app_session_id = "old";
  auto value = live_tuning::describe(c, s, true);
  EXPECT_EQ(value["supported"], false);
  EXPECT_EQ(value["applied_bitrate_kbps"], 0);
  s.session_generation = 2;
  s.app_session_id = "new";
  value = live_tuning::describe(c, s, true);
  EXPECT_EQ(value["state"], "applying");
  EXPECT_EQ(value["applied_bitrate_kbps"], 14000);
  c.session_exclusive = false;
  EXPECT_EQ(live_tuning::describe(c, s, true)["supported"], false);
}

TEST_F(LiveTuningTest, FullReplacementRejectsAnOversizedOrStaleSnapshot) {
  const auto path = config::sunshine.config_file;
  const auto revision = configuration_store::revision(path, true);
  ASSERT_EQ(configuration_store::patch(path, {{"stream_audio", "disabled"}}, revision), configuration_store::result::committed);
  EXPECT_EQ(configuration_store::replace(path, "adaptive_bitrate_enabled = disabled\n", revision), configuration_store::result::conflict);
  EXPECT_EQ(configuration_store::replace(path, std::string(4 * 1024 * 1024 + 1, 'x')), configuration_store::result::failed);
  EXPECT_NE(configuration_store::revision(path, true), "");
}

TEST_F(LiveTuningTest, ExternalEditRefreshesConflictRevision) {
  auto authority = doctor_actions::acquire_admin_global_control();
  const auto old = configuration_store::revision(config::sunshine.config_file, true);
  ASSERT_TRUE(private_state_file::write_atomic(config::sunshine.config_file, "adaptive_bitrate_enabled = enabled\nstream_audio = disabled\n"));
  const auto result = live_tuning::set_enabled(authority, false, old);
  ASSERT_EQ(result["http_status"], 412);
  const auto fresh = result["live_tuning"]["configuration_revision"].get<std::string>();
  EXPECT_NE(old, fresh);
  EXPECT_TRUE(live_tuning::set_enabled(authority, false, fresh)["status"].get<bool>());
}

TEST_F(LiveTuningTest, MatchesTheSharedClientFixtures) {
  std::ifstream input(std::filesystem::path(POLARIS_SOURCE_DIR) / "tests/fixtures/live-tuning-v1.json");
  ASSERT_TRUE(input.good());
  for (const auto &row : nlohmann::json::parse(input)) {
    const auto &expected = row["live_tuning"];
    adaptive_bitrate::state_t c;
    c.session_exclusive = true;
    c.session_generation = expected["session_generation"];
    c.app_session_id = expected["app_session_id"];
    c.enabled = expected["runtime_enabled"];
    c.runtime_update_supported = expected["supported"];
    c.base_bitrate_kbps = expected["quality_limit_kbps"];
    c.target_bitrate_kbps = expected["requested_bitrate_kbps"];
    c.applied_bitrate_kbps = expected["applied_bitrate_kbps"];
    c.feedback_initialized = row["name"] != "measuring";
    c.reason = expected["reason"];
    stream_stats::stats_t stats;
    stats.streaming = row["streaming"];
    stats.session_generation = c.session_generation;
    stats.app_session_id = c.app_session_id;
    const auto actual = live_tuning::describe(c, stats, expected["enabled"]);
    for (const auto &[key, value] : actual.items()) EXPECT_EQ(value, expected[key]) << row["name"] << ": " << key;
  }
}

TEST_F(LiveTuningTest, ReadRetainsTheRevisionOfItsContentsAcrossAnExternalCommit) {
  const auto observed = configuration_store::read(config::sunshine.config_file);
  ASSERT_TRUE(observed);
  const auto replacement = observed->contents + "stream_audio = disabled\n";
  ASSERT_TRUE(private_state_file::write_atomic(config::sunshine.config_file, replacement));
  // This is the read used by GET /api/config. An external commit cannot rebase
  // its earlier contents onto a later revision before that response is sent.
  EXPECT_EQ(configuration_store::replace(config::sunshine.config_file, observed->contents, observed->revision),
    configuration_store::result::conflict);
  const auto current = configuration_store::read(config::sunshine.config_file);
  ASSERT_TRUE(current);
  EXPECT_EQ(current->contents, replacement);
  EXPECT_NE(current->revision, observed->revision);
}
