/**
 * @file tests/unit/platform/test_desktop_takeover.cpp
 * @brief Pure parsing and verification coverage for Hyprland takeover recovery.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "src/platform/linux/desktop_takeover.h"

#include <string>
#include <utility>
#include <vector>

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
  EXPECT_FALSE(desktop_takeover::special_workspace_refusal(unsafe_special))
    << "A name with a space was never addressable, and stays a malformed report";
}

TEST(DesktopTakeover, RefusesSpecialNamesHyprctlReadsAsRequestSyntax) {
  // hyprctl picks its request by substring, so each of these would turn
  // `hyprctl dispatch moveworkspacetomonitor <name> <monitor>` into
  // something else: a batch, another program's socket, or nothing at all.
  const std::vector<std::pair<std::string, std::string>> breakouts {
    {"special:a/--batch;x", "'/'"},
    {"special:x/hyprpaper", "'/'"},
    {"special:x/hyprsunset", "'/'"},
    {"special:x/instances", "'/'"},
    {"special:mail/web", "'/'"},
    {"special:a;b", "';'"},
    {"special:a--batch", "'--'"},
    {"special:a\\b", "'\\'"},
    {"special:[x]", "'['"},
    {"special:x]", "']'"},
  };
  for (const auto &[name, token] : breakouts) {
    const desktop_takeover::workspace_state_t workspace {-98, name, "DP-3"};
    EXPECT_FALSE(desktop_takeover::workspace_selector(workspace)) << name;
    const auto refusal = desktop_takeover::special_workspace_refusal(workspace);
    ASSERT_TRUE(refusal) << name;
    EXPECT_NE(refusal->find(token), std::string::npos) << name << ": " << *refusal;
  }
}

TEST(DesktopTakeover, KeepsOrdinarySpecialNamesAddressable) {
  for (const std::string name : {
         "special:scratch",
         "special:special",
         "special:term-1",
         "special:a-b-c",
         "special:notes_2",
         "special:v1.2",
         "special:Magic",
       }) {
    const desktop_takeover::workspace_state_t workspace {-98, name, "DP-3"};
    EXPECT_EQ(desktop_takeover::workspace_selector(workspace), name);
    EXPECT_FALSE(desktop_takeover::special_workspace_refusal(workspace)) << name;
  }

  const desktop_takeover::workspace_state_t named_regular {4, "web/mail;x", "DP-3"};
  EXPECT_EQ(desktop_takeover::workspace_selector(named_regular), "4")
    << "A regular workspace is selected by id, so its name never reaches hyprctl";
  EXPECT_FALSE(desktop_takeover::special_workspace_refusal(named_regular));
}

TEST(DesktopTakeover, ReportsARefusedSpecialWorkspaceAndNeverReplaysIt) {
  const auto workspaces = desktop_takeover::parse_workspaces(R"json([
    {"id":1,"name":"1","monitor":"DP-3"},
    {"id":-98,"name":"special:a/--batch;x","monitor":"DP-3"},
    {"id":-99,"name":"special:scratch","monitor":"DP-3"}
  ])json");
  ASSERT_TRUE(workspaces) << "One misnamed special workspace must not hide the whole layout";
  ASSERT_EQ(workspaces->size(), 3u);
  EXPECT_FALSE(desktop_takeover::workspace_selector((*workspaces)[1]));
  EXPECT_TRUE(desktop_takeover::special_workspace_refusal((*workspaces)[1]));
  EXPECT_EQ(desktop_takeover::workspace_selector((*workspaces)[2]), "special:scratch");

  desktop_takeover::state_t recorded {
    .owner_pid = 42,
    .active = true,
    .target_output = "HEADLESS-POLARIS-42-1",
    .fallback_monitor = "DP-3",
    .monitors = {{"DP-3", true}},
    .workspaces = {{1, "1", "DP-3"}, {-98, "special:a/--batch;x", "DP-3"}},
  };
  const auto replayed = desktop_takeover::parse_state(desktop_takeover::serialize_state(recorded));
  ASSERT_TRUE(replayed)
    << "A record an older build wrote with such a name must still restore everything else";
  const std::vector<desktop_takeover::workspace_state_t> movable {{1, "1", "DP-3"}};
  EXPECT_EQ(replayed->workspaces, movable) << "The refused name is never replayed to hyprctl";

  recorded.workspaces = {{-98, "special:a/--batch;x", "DP-3"}};
  EXPECT_FALSE(desktop_takeover::parse_state(desktop_takeover::serialize_state(recorded)))
    << "A record with nothing Polaris can move back is still malformed";

  recorded.workspaces = {{1, "1", "DP-3"}, {-99, "special:bad name", "DP-3"}};
  EXPECT_FALSE(desktop_takeover::parse_state(desktop_takeover::serialize_state(recorded)))
    << "A name that was never addressable still makes the record malformed";
}

TEST(DesktopTakeover, RestoreLeavesARefusedSpecialWorkspaceToHyprland) {
  desktop_takeover::state_t state {
    .active = true,
    .target_output = "HEADLESS-POLARIS-42-1",
    .fallback_monitor = "DP-3",
    .monitors = {{"DP-3", true}},
    .workspaces = {{1, "1", "DP-3"}},
  };
  EXPECT_TRUE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", "DP-3"}, {-98, "special:a/--batch;x", state.target_output}}
  )) << "A special workspace opened on the stream's output that Polaris cannot name "
        "is left for Hyprland to move when the output closes";
  EXPECT_FALSE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", "DP-3"}, {-99, "special:scratch", state.target_output}}
  )) << "A special workspace Polaris can move must still be off the output first";
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
