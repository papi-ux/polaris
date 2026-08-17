/**
 * @file tests/unit/platform/test_virtual_display.cpp
 * @brief Test Linux virtual display backend detection helpers.
 */
#include "../../tests_common.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __linux__
  #include <src/platform/linux/virtual_display.h>

TEST(VirtualDisplayTests, HyprlandMonitorModeReadsBackTheGeometryTheCompositorActuallyHas) {
  // The whole point of reading this back is that hyprctl's exit status lies:
  // Hyprland 0.56 answers `hyprctl keyword` with "unknown request" and still
  // exits 0, so a rejected mode set looked like success and the stream was
  // captured at the compositor default and scaled (#444).
  constexpr std::string_view monitors = R"([
    {"name":"DP-1","width":3440,"height":1440,"refreshRate":99.982},
    {"name":"POLARIS-HEADLESS-4242-0","width":1920,"height":1080,"refreshRate":60.0}
  ])";

  const auto actual = virtual_display::hyprland_monitor_mode(monitors, "POLARIS-HEADLESS-4242-0");
  ASSERT_TRUE(actual.has_value());
  EXPECT_EQ(actual->width, 1920);
  EXPECT_EQ(actual->height, 1080);
  EXPECT_NEAR(actual->refresh_hz, 60.0, 0.001);

  // This is the reported failure in miniature: 2400x1080 was requested and the
  // output is still 1920x1080, which the caller must be able to notice.
  EXPECT_NE(actual->width, 2400);

  const auto other = virtual_display::hyprland_monitor_mode(monitors, "DP-1");
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->width, 3440);
}

TEST(VirtualDisplayTests, HyprlandMonitorModeRefusesAbsentOutputsAndUnusableGeometry) {
  constexpr std::string_view monitors = R"([{"name":"DP-1","width":3440,"height":1440}])";

  // Absent output: no answer, not a zero-sized one.
  EXPECT_FALSE(virtual_display::hyprland_monitor_mode(monitors, "POLARIS-HEADLESS-4242-0").has_value());

  // Present but unusable geometry must not read as a successful mode set.
  EXPECT_FALSE(
    virtual_display::hyprland_monitor_mode(R"([{"name":"X","width":0,"height":0}])", "X").has_value()
  );
  EXPECT_FALSE(
    virtual_display::hyprland_monitor_mode(R"([{"name":"X"}])", "X").has_value()
  );

  // Malformed or non-array input is a gap, not a match.
  EXPECT_FALSE(virtual_display::hyprland_monitor_mode("not json", "X").has_value());
  EXPECT_FALSE(virtual_display::hyprland_monitor_mode("{}", "X").has_value());
  EXPECT_FALSE(virtual_display::hyprland_monitor_mode("", "X").has_value());
}

TEST(VirtualDisplayTests, BackendDetectionLogCacheOnlySignalsOnFirstObservationAndChanges) {
  virtual_display::backend_detection_log_cache_t cache;

  EXPECT_TRUE(cache.note(virtual_display::backend_e::KSCREEN_DOCTOR));
  EXPECT_FALSE(cache.note(virtual_display::backend_e::KSCREEN_DOCTOR));
  EXPECT_TRUE(cache.note(virtual_display::backend_e::WAYLAND_WLR));
  EXPECT_FALSE(cache.note(virtual_display::backend_e::WAYLAND_WLR));
}

TEST(VirtualDisplayTests, UnavailableReasonMapsEveryProbedState) {
  using virtual_display::backend_e;
  using virtual_display::unavailable_reason_for;

  // Usable backends carry no reason.
  EXPECT_EQ(unavailable_reason_for(backend_e::EVDI, false, false), "");
  EXPECT_EQ(unavailable_reason_for(backend_e::WAYLAND_WLR, false, false), "");
  EXPECT_EQ(unavailable_reason_for(backend_e::KSCREEN_DOCTOR, false, true), "");

  // kscreen-doctor without a configured streaming output names the missing config key.
  const auto kscreen = unavailable_reason_for(backend_e::KSCREEN_DOCTOR, false, false);
  EXPECT_NE(kscreen.find("linux_streaming_output"), std::string::npos);

  // A loaded-but-uncreatable EVDI names the actionable fix.
  const auto evdi_blocked = unavailable_reason_for(backend_e::NONE, true, false);
  EXPECT_NE(evdi_blocked.find("initial_device_count"), std::string::npos);
  EXPECT_NE(evdi_blocked.find("/sys/devices/evdi/add"), std::string::npos);

  // No backend at all still yields a non-empty explanation.
  const auto none = unavailable_reason_for(backend_e::NONE, false, false);
  EXPECT_FALSE(none.empty());
  EXPECT_EQ(none.find("initial_device_count"), std::string::npos);
}

TEST(VirtualDisplayTests, KscreenDoctorRequiresConfiguredStreamingOutput) {
  EXPECT_FALSE(virtual_display::backend_has_required_configuration(
    virtual_display::backend_e::NONE,
    "HDMI-A-1"));
  EXPECT_TRUE(virtual_display::backend_has_required_configuration(
    virtual_display::backend_e::EVDI,
    ""));
  EXPECT_TRUE(virtual_display::backend_has_required_configuration(
    virtual_display::backend_e::WAYLAND_WLR,
    ""));
  EXPECT_FALSE(virtual_display::backend_has_required_configuration(
    virtual_display::backend_e::KSCREEN_DOCTOR,
    ""));
  EXPECT_TRUE(virtual_display::backend_has_required_configuration(
    virtual_display::backend_e::KSCREEN_DOCTOR,
    "HDMI-A-1"));
}

TEST(VirtualDisplayTests, WaylandProbeAllowsPreInitWaylandEnvironment) {
  EXPECT_TRUE(virtual_display::wayland_backend_probe_allowed(false, "wayland-1"));
  EXPECT_TRUE(virtual_display::wayland_backend_probe_allowed(true, ""));
  EXPECT_FALSE(virtual_display::wayland_backend_probe_allowed(false, ""));
}

TEST(VirtualDisplayTests, HyprlandOutputNameIsPolarisOwnedAndProcessScoped) {
  EXPECT_EQ(
    virtual_display::hyprland_output_name_for_pid(4242, 0),
    "POLARIS-HEADLESS-4242-0"
  );
  EXPECT_TRUE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-4242-0"));
  EXPECT_FALSE(virtual_display::hyprland_output_is_polaris_owned("HEADLESS-1"));
  EXPECT_FALSE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-"));
  EXPECT_FALSE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-user"));
  EXPECT_FALSE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-4242-"));
  EXPECT_FALSE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-4242-a"));
}

TEST(VirtualDisplayTests, HyprlandSlotSuffixSeparatesConcurrentDisplaysInOneProcess) {
  // A streaming session and the web UI each create a display in the same
  // process; distinct slots are what keep them from colliding on one connector.
  EXPECT_NE(
    virtual_display::hyprland_output_name_for_pid(4242, 0),
    virtual_display::hyprland_output_name_for_pid(4242, 1)
  );
  EXPECT_TRUE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-4242-1"));

  // An output left behind by a Polaris that predates the slot suffix stays
  // removable, so an upgrade does not strand it in the compositor.
  EXPECT_TRUE(virtual_display::hyprland_output_is_polaris_owned("POLARIS-HEADLESS-4242"));
}

TEST(VirtualDisplayTests, HyprlandMonitorLookupDoesNotSelectExistingHeadlessOutput) {
  constexpr auto monitors = R"json([
    {"name":"HEADLESS-1"},
    {"name":"POLARIS-HEADLESS-4242-0"}
  ])json";

  EXPECT_TRUE(virtual_display::hyprland_monitors_contain_output(
    monitors,
    "POLARIS-HEADLESS-4242-0"
  ));
  EXPECT_FALSE(virtual_display::hyprland_monitors_contain_output(
    monitors,
    "POLARIS-HEADLESS-4242-1"
  ));
  EXPECT_FALSE(virtual_display::hyprland_monitors_contain_output(
    monitors,
    "POLARIS-HEADLESS-9999-0"
  ));
  EXPECT_FALSE(virtual_display::hyprland_monitors_contain_output(
    "not json",
    "POLARIS-HEADLESS-4242-0"
  ));
}

TEST(VirtualDisplayTests, PersistedStateTracksEveryConcurrentDisplay) {
  // A streaming session and the web UI each own a display in one process, so
  // both have to survive a round trip through the state file.
  constexpr auto state = R"json({
    "displays": [
      {"pid":4242,"output_name":"POLARIS-HEADLESS-4242-0","width":1920,"height":1080,
       "fps":60,"active":true,"backend":"wayland_wlr","device_path":""},
      {"pid":4242,"output_name":"POLARIS-HEADLESS-4242-1","width":2560,"height":1440,
       "fps":120,"active":true,"backend":"wayland_wlr","device_path":""}
    ]
  })json";

  const auto displays = virtual_display::parse_persisted_displays(state);
  ASSERT_EQ(displays.size(), 2u);
  EXPECT_EQ(displays[0].owner_pid, 4242);
  EXPECT_EQ(displays[0].display.output_name, "POLARIS-HEADLESS-4242-0");
  EXPECT_EQ(displays[0].display.width, 1920);
  EXPECT_EQ(displays[0].display.backend, virtual_display::backend_e::WAYLAND_WLR);
  EXPECT_EQ(displays[1].display.output_name, "POLARIS-HEADLESS-4242-1");
  EXPECT_EQ(displays[1].display.fps, 120);
}

TEST(VirtualDisplayTests, PersistedStateStillReadsPreListSingleDisplayDocument) {
  // Written by a Polaris that predates concurrent displays; an upgrade has to
  // keep reading it or the output it recorded is never cleaned up.
  constexpr auto legacy = R"json({
    "pid":1234,"output_name":"POLARIS-HEADLESS-1234","width":1920,"height":1080,
    "fps":60,"active":true,"backend":"wayland_wlr","device_path":""
  })json";

  const auto displays = virtual_display::parse_persisted_displays(legacy);
  ASSERT_EQ(displays.size(), 1u);
  EXPECT_EQ(displays[0].owner_pid, 1234);
  EXPECT_EQ(displays[0].display.output_name, "POLARIS-HEADLESS-1234");
}

TEST(VirtualDisplayTests, PersistedStateDropsUnusableRecords) {
  constexpr auto state = R"json({
    "displays": [
      {"pid":1,"output_name":"POLARIS-HEADLESS-1-0","active":false,"backend":"wayland_wlr"},
      {"pid":2,"output_name":"","active":true,"backend":"wayland_wlr"},
      {"pid":3,"output_name":"POLARIS-HEADLESS-3-0","active":true,"backend":"none"},
      {"pid":4,"output_name":"POLARIS-HEADLESS-4-0","active":true,"backend":"evdi"}
    ]
  })json";

  const auto displays = virtual_display::parse_persisted_displays(state);
  ASSERT_EQ(displays.size(), 1u);
  EXPECT_EQ(displays[0].owner_pid, 4);
  EXPECT_EQ(displays[0].display.backend, virtual_display::backend_e::EVDI);

  EXPECT_TRUE(virtual_display::parse_persisted_displays("not json").empty());
  EXPECT_TRUE(virtual_display::parse_persisted_displays("").empty());
  EXPECT_TRUE(virtual_display::parse_persisted_displays("[]").empty());
}

TEST(VirtualDisplayTests, PersistedDisplayOwnedByThisProcessIsNeverStale) {
  using virtual_display::persisted_display_is_stale;

  // The live sibling case: our own record backs a display someone in this
  // process still holds, so cleanup must leave it alone.
  EXPECT_FALSE(persisted_display_is_stale(4242, 4242, false));
  EXPECT_FALSE(persisted_display_is_stale(4242, 4242, true));

  // Another Polaris is still running with it.
  EXPECT_FALSE(persisted_display_is_stale(1234, 4242, true));

  // Left behind by a process that died.
  EXPECT_TRUE(persisted_display_is_stale(1234, 4242, false));
  EXPECT_TRUE(persisted_display_is_stale(0, 4242, false));
}

TEST(VirtualDisplayTests, FailedVirtualDisplayRequestNeverStreamsPhysicalOutput) {
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/process.cpp";
  std::ifstream input {path};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const auto source = buffer.str();
  ASSERT_FALSE(source.empty());

  const auto request = source.find("const bool should_use_linux_virtual_display");
  const auto fallback_end = source.find("} else if (using_headless_cage)", request);
  ASSERT_NE(request, std::string::npos);
  ASSERT_NE(fallback_end, std::string::npos);
  const auto launch_path = source.substr(request, fallback_end - request);

  EXPECT_EQ(launch_path.find("streams the host's current output instead"), std::string::npos);
  EXPECT_NE(launch_path.find("refusing to stream the host's current output"), std::string::npos);

  const auto first_failure = launch_path.find("return 503;");
  ASSERT_NE(first_failure, std::string::npos);
  EXPECT_NE(launch_path.find("return 503;", first_failure + 1), std::string::npos);
}
#else
TEST(VirtualDisplayTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only virtual display tests";
}
#endif
