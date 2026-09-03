/**
 * @file tests/unit/platform/test_desktop_takeover.cpp
 * @brief Pure parsing and verification coverage for Hyprland takeover recovery.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "src/platform/linux/desktop_takeover.h"

TEST(DesktopTakeover, ParsesMonitorPowerAndWorkspacePlacement) {
  const auto monitors = desktop_takeover::parse_monitors(R"json([
    {"name":"DP-3","dpmsStatus":true,"focused":true},
    {"name":"HEADLESS-POLARIS-42-1","dpmsStatus":true}
  ])json");
  ASSERT_TRUE(monitors);
  ASSERT_EQ(monitors->size(), 2u);
  EXPECT_EQ(monitors->front().name, "DP-3");
  EXPECT_TRUE(monitors->front().dpms_on);

  const auto workspaces = desktop_takeover::parse_workspaces(R"json([
    {"id":1,"name":"1","monitor":"DP-3"},
    {"id":-99,"name":"special:scratch","monitor":"DP-3"}
  ])json");
  ASSERT_TRUE(workspaces);
  ASSERT_EQ(workspaces->size(), 2u);
  EXPECT_EQ(desktop_takeover::workspace_selector(workspaces->front()), "1");
  EXPECT_EQ(desktop_takeover::workspace_selector(workspaces->back()), "special:scratch");
}

TEST(DesktopTakeover, RejectsUnsafeOrUnaddressableWorkspaceIdentity) {
  EXPECT_FALSE(desktop_takeover::parse_workspaces(
    R"([{"id":0,"name":"","monitor":"DP-3"}])"
  ));
  EXPECT_FALSE(desktop_takeover::parse_workspaces(
    "[{\"id\":1,\"name\":\"1\",\"monitor\":\"DP-3\\nDP-4\"}]"
  ));
  desktop_takeover::workspace_state_t unsafe_special {
    -99,
    "special:bad name",
    "DP-3",
  };
  EXPECT_FALSE(desktop_takeover::workspace_selector(unsafe_special));
}

TEST(DesktopTakeover, RoundTripsDurableRecoveryState) {
  desktop_takeover::state_t expected {
    .owner_pid = 42,
    .active = true,
    .target_output = "HEADLESS-POLARIS-42-1",
    .fallback_monitor = "DP-3",
    .monitors = {{"DP-3", true}, {"HDMI-A-1", true}},
    .workspaces = {{1, "1", "DP-3"}, {-99, "special:scratch", "HDMI-A-1"}},
  };
  const auto parsed = desktop_takeover::parse_state(
    desktop_takeover::serialize_state(expected)
  );
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->owner_pid, expected.owner_pid);
  EXPECT_EQ(parsed->active, expected.active);
  EXPECT_EQ(parsed->target_output, expected.target_output);
  EXPECT_EQ(parsed->fallback_monitor, expected.fallback_monitor);
  EXPECT_EQ(parsed->monitors, expected.monitors);
  EXPECT_EQ(parsed->workspaces, expected.workspaces);
}

TEST(DesktopTakeover, OnlyInactiveRecoveryDocumentMayBeReplaced) {
  desktop_takeover::state_t inactive;
  inactive.active = false;
  EXPECT_TRUE(desktop_takeover::recovery_document_allows_takeover(
    desktop_takeover::serialize_state(inactive)
  ));

  desktop_takeover::state_t active {
    .owner_pid = 42,
    .active = true,
    .target_output = "HEADLESS-POLARIS-42-1",
    .fallback_monitor = "DP-3",
    .monitors = {{"DP-3", true}},
    .workspaces = {{1, "1", "DP-3"}},
  };
  EXPECT_FALSE(desktop_takeover::recovery_document_allows_takeover(
    desktop_takeover::serialize_state(active)
  ));
  EXPECT_FALSE(desktop_takeover::recovery_document_allows_takeover("not-json"));
  EXPECT_FALSE(desktop_takeover::recovery_document_allows_takeover("{}"));
}

TEST(DesktopTakeover, VerifiesTakeoverAndRestoreByExactWorkspaceIdentity) {
  desktop_takeover::state_t state {
    .active = true,
    .target_output = "HEADLESS-POLARIS-42-1",
    .fallback_monitor = "DP-3",
    .monitors = {{"DP-3", true}},
    .workspaces = {{1, "1", "DP-3"}, {2, "2", "DP-3"}},
  };
  EXPECT_TRUE(desktop_takeover::takeover_layout_matches(
    state,
    {{1, "1", state.target_output}, {2, "2", state.target_output}}
  ));
  EXPECT_FALSE(desktop_takeover::takeover_layout_matches(
    state,
    {{1, "1", state.target_output}, {2, "2", "DP-3"}}
  ));
  EXPECT_TRUE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", "DP-3"}, {3, "3", "DP-3"}}
  )) << "A workspace closed during the session does not make recovery fail";
  EXPECT_FALSE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", state.target_output}, {2, "2", "DP-3"}}
  ));
  EXPECT_FALSE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", "DP-3"}, {2, "2", "DP-3"}, {3, "3", state.target_output}}
  )) << "No newly created workspace may remain on an output Polaris will destroy";
}

TEST(DesktopTakeover, InactiveTombstoneNeedsNoTopologyDetails) {
  const auto parsed = desktop_takeover::parse_state(R"({"version":1,"active":false})");
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->active);
}

#endif
