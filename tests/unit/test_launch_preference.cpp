#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {
  std::unique_ptr<crypto::named_cert_t> client_cert() {
    auto cert = std::make_unique<crypto::named_cert_t>();
    cert->name = "test-client";
    cert->uuid = "remote-client";
    cert->perm = crypto::PERM::_game_control;
    return cert;
  }

  nvhttp::args_t launch_args(std::string preference = {}) {
    nvhttp::args_t args;
    args.emplace("rikey", std::string(crypto::cipher::key_size * 2, '0'));
    args.emplace("rikeyid", "1");
    args.emplace("mode", "1920x1080x120");
    if (!preference.empty()) {
      args.emplace("profilePreference", std::move(preference));
    }
    return args;
  }
}

TEST(LaunchPreference, MissingPreferenceDefaultsToAuto) {
  auto cert = client_cert();
  const auto session = nvhttp::make_launch_session(true, false, launch_args(), cert.get());

  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->profile_preference, "auto");
  EXPECT_EQ(session->requested_fps, 120000);
}

TEST(LaunchPreference, CamelCaseHighFpsPropagatesToLaunchSession) {
  auto cert = client_cert();
  const auto session = nvhttp::make_launch_session(
    true,
    false,
    launch_args("HIGH_FPS"),
    cert.get()
  );

  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->profile_preference, "high_fps");
  EXPECT_EQ(session->requested_fps, 120000);
}

TEST(LaunchPreference, KnownNonTrialPreferencesRemainDistinct) {
  for (const auto *preference : {"quality", "stability"}) {
    auto cert = client_cert();
    const auto session = nvhttp::make_launch_session(
      true,
      false,
      launch_args(preference),
      cert.get()
    );

    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->profile_preference, preference);
  }
}

TEST(LaunchPreference, UnknownPreferenceFailsSafeToAuto) {
  auto cert = client_cert();
  const auto session = nvhttp::make_launch_session(
    true,
    false,
    launch_args("turbo"),
    cert.get()
  );

  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->profile_preference, "auto");
}

TEST(LaunchPreference, HighFpsBypassesOnlySoftActiveHistoryCap) {
  EXPECT_FALSE(proc::should_apply_history_safe_fps_cap_for_tests(
    true, 60.0, false, "high_fps"
  ));

  for (const auto *preference : {"auto", "quality", "stability"}) {
    EXPECT_TRUE(proc::should_apply_history_safe_fps_cap_for_tests(
      true, 60.0, false, preference
    )) << preference;
  }

  EXPECT_FALSE(proc::should_apply_history_safe_fps_cap_for_tests(
    false, 60.0, false, "auto"
  ));
  EXPECT_FALSE(proc::should_apply_history_safe_fps_cap_for_tests(
    true, 0.0, false, "auto"
  ));
  EXPECT_FALSE(proc::should_apply_history_safe_fps_cap_for_tests(
    true, 60.0, true, "auto"
  ));
  EXPECT_TRUE(proc::should_apply_history_safe_fps_cap_for_tests(
    true, 60.0, false, "HIGH_FPS"
  ));
}
