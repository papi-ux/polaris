/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Unit tests for adaptive bitrate controller behavior.
 */

#include "src/adaptive_bitrate.h"
#include "src/config.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {
  void enable_controller(int base_bitrate_kbps = 20000) {
    config::video.adaptive_bitrate.enabled = true;
    config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
    config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    adaptive_bitrate::set_base_bitrate(base_bitrate_kbps);
  }
}

TEST(AdaptiveBitrateController, ReducesTargetOnNetworkPressure) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ("network_pressure", state.state);
}

TEST(AdaptiveBitrateController, ReportsFramePacingWithoutLoweringBitrate) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.75, 0.05, 0.02, 3.0, 4.0, 28.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_EQ(state.base_bitrate_kbps, state.target_bitrate_kbps);
  EXPECT_EQ("frame_pacing_observed", state.state);
}

TEST(AdaptiveBitrateController, ReducesTargetOnEncoderPressure) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ("encoder_pressure", state.state);
}

TEST(AdaptiveBitrateController, ClampsBaseToConfiguredBounds) {
  enable_controller(100000);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(50000, state.base_bitrate_kbps);
  EXPECT_EQ(50000, state.target_bitrate_kbps);
}
