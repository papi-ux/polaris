/**
 * @file tests/unit/platform/test_display_topology.cpp
 * @brief Test the kscreen mode read-back that confirms a mode request landed.
 */
#include <gtest/gtest.h>
#include <src/platform/linux/display_topology.h>
#include <string>

using namespace std::literals;

namespace {
  // Shape taken from real `kscreen-doctor -j` output, including the detail that
  // makes the read-back non-trivial: kscreen reports 59.987 for a mode it names
  // 60, and the current mode is named by id rather than inline.
  constexpr auto sample = R"({
    "features": 255,
    "outputs": [
      {
        "name": "HDMI-A-1",
        "connected": true,
        "enabled": true,
        "currentModeId": "7",
        "modes": [
          {"id": "7", "name": "1920x1080@60", "refreshRate": 59.98699951171875,
           "size": {"height": 1080, "width": 1920}}
        ]
      },
      {
        "name": "DP-2",
        "connected": true,
        "enabled": true,
        "currentModeId": "2",
        "modes": [
          {"id": "1", "name": "3840x2160@60", "refreshRate": 59.96799850463867,
           "size": {"height": 2160, "width": 3840}},
          {"id": "2", "name": "2560x1440@120", "refreshRate": 119.998,
           "size": {"height": 1440, "width": 2560}}
        ]
      }
    ]
  })"sv;
}  // namespace

TEST(DisplayTopologyModeReadBack, ResolvesTheCurrentModeById) {
  EXPECT_EQ(display_topology::current_output_mode(sample, "DP-2"), "2560x1440@120Hz");
}

TEST(DisplayTopologyModeReadBack, RoundsTheRefreshRateKscreenReports) {
  // 59.987 is the same mode a user and the request string both call 60.
  EXPECT_EQ(display_topology::current_output_mode(sample, "HDMI-A-1"), "1920x1080@60Hz");
}

TEST(DisplayTopologyModeReadBack, ReadsEachOutputSeparately) {
  // Two outputs, different current modes. Reading the wrong one would report a
  // mismatch that never happened, or hide one that did.
  EXPECT_NE(
    display_topology::current_output_mode(sample, "DP-2"),
    display_topology::current_output_mode(sample, "HDMI-A-1")
  );
}

TEST(DisplayTopologyModeReadBack, ReturnsNothingForAnUnknownOutput) {
  EXPECT_TRUE(display_topology::current_output_mode(sample, "DP-9").empty());
}

TEST(DisplayTopologyModeReadBack, ReturnsNothingWhenTheModeIdDoesNotResolve) {
  constexpr auto dangling = R"({"outputs":[{"name":"DP-1","currentModeId":"99",
    "modes":[{"id":"1","size":{"width":1920,"height":1080},"refreshRate":60.0}]}]})"sv;

  EXPECT_TRUE(display_topology::current_output_mode(dangling, "DP-1").empty());
}

TEST(DisplayTopologyModeReadBack, TreatsUnusableInputAsUnknownRatherThanAMismatch) {
  // Every one of these must read as "could not confirm" and not as "the mode is
  // wrong". Reporting a silent failure that was never observed would be its own
  // false alarm.
  EXPECT_TRUE(display_topology::current_output_mode(""sv, "DP-1").empty());
  EXPECT_TRUE(display_topology::current_output_mode("not json"sv, "DP-1").empty());
  EXPECT_TRUE(display_topology::current_output_mode("[]"sv, "DP-1").empty());
  EXPECT_TRUE(display_topology::current_output_mode(R"({"outputs":"nope"})"sv, "DP-1").empty());
  EXPECT_TRUE(display_topology::current_output_mode(sample, ""sv).empty());
}

TEST(DisplayTopologyModeReadBack, OmitsRefreshWhenKscreenDoesNotReportIt) {
  constexpr auto no_refresh = R"({"outputs":[{"name":"DP-1","currentModeId":"1",
    "modes":[{"id":"1","size":{"width":1920,"height":1080}}]}]})"sv;

  EXPECT_EQ(display_topology::current_output_mode(no_refresh, "DP-1"), "1920x1080");
}

TEST(DisplayTopologyModeReadBack, RejectsADegenerateModeSize) {
  constexpr auto zero_size = R"({"outputs":[{"name":"DP-1","currentModeId":"1",
    "modes":[{"id":"1","size":{"width":0,"height":0},"refreshRate":60.0}]}]})"sv;

  EXPECT_TRUE(display_topology::current_output_mode(zero_size, "DP-1").empty());
}
