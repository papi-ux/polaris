/**
 * @file tests/unit/platform/test_desktop_takeover.cpp
 * @brief Pure parsing and verification coverage for Hyprland takeover recovery.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "src/platform/linux/desktop_takeover.h"

TEST(DesktopTakeover, ParsesMonitorPowerAndWorkspacePlacement) {
  const auto monitors = desktop_takeover::parse_monitors(R"json([
    {"name":"DP-3","dpmsStatus":true,"focused":true,"x":2048,"y":0,"width":2560,"height":1440,"refreshRate":240.002,"scale":1.25},
    {"name":"HEADLESS-POLARIS-42-1","dpmsStatus":true}
  ])json");
  ASSERT_TRUE(monitors);
  ASSERT_EQ(monitors->size(), 2u);
  EXPECT_EQ(monitors->front().name, "DP-3");
  EXPECT_TRUE(monitors->front().dpms_on);
  EXPECT_EQ(monitors->front().x, 2048);
  EXPECT_EQ(monitors->front().y, 0);
  EXPECT_EQ(monitors->front().mode, "2560x1440@240");
  EXPECT_DOUBLE_EQ(monitors->front().scale, 1.25);
  EXPECT_EQ(monitors->back().mode, "") << "A monitor that reports no mode restores only what it reported";

  const auto workspaces = desktop_takeover::parse_workspaces(R"json([
    {"id":1,"name":"1","monitor":"DP-3"},
    {"id":-99,"name":"special:scratch","monitor":"DP-3"}
  ])json");
  ASSERT_TRUE(workspaces);
  ASSERT_EQ(workspaces->size(), 2u);
  EXPECT_EQ(desktop_takeover::workspace_selector(workspaces->front()), "1");
  EXPECT_EQ(desktop_takeover::workspace_selector(workspaces->back()), "special:scratch");
  EXPECT_EQ(workspaces->front().windows, 1) << "An unreported window count is treated as non-empty";

  const auto counted = desktop_takeover::parse_workspaces(
    R"json([{"id":5,"name":"5","monitor":"DP-3","windows":3}])json");
  ASSERT_TRUE(counted);
  EXPECT_EQ(counted->front().windows, 3);
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
    .monitors = {{"DP-3", true, 2048, 0, "2560x1440@240", 1.25}, {"HDMI-A-1", true}},
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

TEST(DesktopTakeover, RoundTripsRecoveryStateWithNoWindowedWorkspaces) {
  // A desktop with nothing open is taken over with no recorded workspaces;
  // after a crash mid-session, recovery must still read the record and power
  // the monitors back on rather than call the document malformed.
  desktop_takeover::state_t expected {
    .owner_pid = 42,
    .active = true,
    .target_output = "HEADLESS-POLARIS-42-1",
    .fallback_monitor = "DP-3",
    .monitors = {{"DP-3", true}},
    .workspaces = {},
  };
  const auto parsed = desktop_takeover::parse_state(
    desktop_takeover::serialize_state(expected)
  );
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->active);
  EXPECT_EQ(parsed->monitors, expected.monitors);
  EXPECT_TRUE(parsed->workspaces.empty());
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
  EXPECT_TRUE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", "DP-3"}, {2, "2", "DP-3"}, {3, "3", state.target_output, 0}}
  )) << "Hyprland backfills an empty workspace the moment the last one leaves; it dies with the output";
  EXPECT_FALSE(desktop_takeover::restored_layout_matches(
    state,
    {{1, "1", "DP-3"}, {2, "2", "DP-3"}, {3, "3", state.target_output, 2}}
  )) << "A backfilled workspace that gained windows must still move off the target";
}

TEST(DesktopTakeover, TranslatesClassicDispatchIntoHyprlandLuaDispatchers) {
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"dpms", "on", "DP-2"}),
    std::optional<std::string> {"hl.dsp.dpms({ action = \"on\", monitor = \"DP-2\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"dpms", "off", "DP-3"}),
    std::optional<std::string> {"hl.dsp.dpms({ action = \"off\", monitor = \"DP-3\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "2", "POLARIS-HEADLESS-42-0"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"2\", monitor = \"POLARIS-HEADLESS-42-0\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:scratch", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:scratch\", monitor = \"DP-2\" })"}
  );
}

TEST(DesktopTakeover, LuaDispatcherQuotesWorkspaceNamesItCannotTrust) {
  // A named special workspace is free text; neither quote nor backslash may
  // end the Lua string early or turn into an escape.
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:a\"b", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:a\\\"b\", monitor = \"DP-2\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:a\\b", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:a\\\\b\", monitor = \"DP-2\" })"}
  );
}

TEST(DesktopTakeover, LuaDispatcherRefusesDispatchesTakeoverNeverIssues) {
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "toggle", "DP-2"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"exec", "firefox"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({}));
}

TEST(DesktopTakeover, TranslatesMonitorLayoutStateForLuaConfig) {
  desktop_takeover::monitor_state_t plain {
    "DP-2",
    true,
  };
  EXPECT_EQ(
    desktop_takeover::lua_monitor_state(plain, false),
    std::optional<std::string> {"hl.monitor({ output = \"DP-2\", disabled = true })"}
  ) << "Disabling carries no geometry: the output is leaving the layout";
  EXPECT_EQ(
    desktop_takeover::lua_monitor_state(plain, true),
    std::optional<std::string> {"hl.monitor({ output = \"DP-2\", disabled = false })"}
  ) << "A monitor that reported no geometry comes back without overriding its rule";

  desktop_takeover::monitor_state_t placed {
    "DP-2",
    true,
    2048,
    0,
    "2560x1440@240",
    1.25,
  };
  EXPECT_EQ(
    desktop_takeover::lua_monitor_state(placed, true),
    std::optional<std::string> {
      "hl.monitor({ output = \"DP-2\", disabled = false, position = \"2048x0\", "
      "mode = \"2560x1440@240\", scale = 1.25 })"
    }
  ) << "Enabling re-states the recorded geometry so a desc: or catch-all rule cannot pull it to defaults";

  desktop_takeover::monitor_state_t quoted {
    "a\"b",
    true,
  };
  EXPECT_EQ(
    desktop_takeover::lua_monitor_state(quoted, false),
    std::optional<std::string> {"hl.monitor({ output = \"a\\\"b\", disabled = true })"}
  );
  desktop_takeover::monitor_state_t unsafe {
    "DP 2",
    true,
  };
  EXPECT_FALSE(desktop_takeover::lua_monitor_state(unsafe, false));
}

TEST(DesktopTakeover, LuaDispatcherRefusesArgumentsItCannotNameSafely) {
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on", "DP-2\n"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on", "DP-2\r"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on", std::string {"DP-2\0", 5}}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on", "DP-2\x7f"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on", "DP 2"}));
  EXPECT_FALSE(desktop_takeover::lua_dispatcher({"dpms", "on", ""}));
}

TEST(DesktopTakeover, LuaDispatcherNeutralizesLuaEscapeAndBreakoutAttempts) {
  // A leading backslash survives as literal text: the quoting escapes it, so
  // Lua cannot read \z, \x22, \u{22} or \34 as an escape inside the string.
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:a\\z", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:a\\\\z\", monitor = \"DP-2\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:a\\x22", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:a\\\\x22\", monitor = \"DP-2\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:a\\u{22}", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:a\\\\u{22}\", monitor = \"DP-2\" })"}
  );
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "special:a\\34", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"special:a\\\\34\", monitor = \"DP-2\" })"}
  );
  // Printable hostile input is nameable, so it must come back quoted shut
  // rather than refused: the quotes are escaped and nothing after them runs.
  EXPECT_EQ(
    desktop_takeover::lua_dispatcher({"moveworkspacetomonitor", "x\")os.execute(\"id\")--", "DP-2"}),
    std::optional<std::string> {"hl.dsp.workspace.move({ workspace = \"x\\\")os.execute(\\\"id\\\")--\", monitor = \"DP-2\" })"}
  );
}

TEST(DesktopTakeover, InactiveTombstoneNeedsNoTopologyDetails) {
  const auto parsed = desktop_takeover::parse_state(R"({"version":1,"active":false})");
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->active);
}

#endif
