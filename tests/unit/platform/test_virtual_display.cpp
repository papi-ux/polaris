/**
 * @file tests/unit/platform/test_virtual_display.cpp
 * @brief Test Linux virtual display backend detection helpers.
 */
#include "../../tests_common.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

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

TEST(VirtualDisplayTests, KscreenJsonSnapshotCarriesExactRestoreFields) {
  constexpr auto output_json = R"json({
    "outputs": [
      {"name":"DP-1","enabled":true,"currentModeId":"7","priority":1},
      {"name":"HDMI-A-1","enabled":false,"currentModeId":"12","priority":0}
    ]
  })json";

  const auto state = virtual_display::kscreen_output_state_from_json(output_json, "HDMI-A-1");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->name, "HDMI-A-1");
  EXPECT_FALSE(state->enabled);
  EXPECT_EQ(state->current_mode_id, "12");
  EXPECT_EQ(state->priority, 0);
  EXPECT_FALSE(virtual_display::kscreen_output_state_from_json(output_json, "DP-9").has_value());
  EXPECT_FALSE(virtual_display::kscreen_output_state_from_json("not json", "DP-1").has_value());
}

TEST(VirtualDisplayTests, KscreenEnableDemotesOnlyADistinctPrimaryOutput) {
  using args_t = std::vector<std::string>;

  // With linux_streaming_output and linux_primary_output both naming DP-1, the
  // argv once told DP-1 to be priority 1 and then 2. kscreen-doctor applies its
  // arguments in order, so the trailing 2 won and another monitor went first.
  EXPECT_EQ(
    virtual_display::kscreen_enable_args("DP-1", "1920x1080@60", "DP-1"),
    (args_t {"kscreen-doctor", "output.DP-1.enable", "output.DP-1.mode.1920x1080@60", "output.DP-1.priority.1"})
  );
  EXPECT_EQ(
    virtual_display::kscreen_enable_args("DP-1", "", "DP-1"),
    (args_t {"kscreen-doctor", "output.DP-1.enable", "output.DP-1.priority.1"})
  );

  // A dummy plug beside a real panel still hands the panel priority 2.
  EXPECT_EQ(
    virtual_display::kscreen_enable_args("HDMI-A-2", "1920x1080@60", "DP-1"),
    (args_t {"kscreen-doctor", "output.HDMI-A-2.enable", "output.HDMI-A-2.mode.1920x1080@60", "output.HDMI-A-2.priority.1", "output.DP-1.priority.2"})
  );
  EXPECT_EQ(
    virtual_display::kscreen_enable_args("HDMI-A-2", "", "DP-1"),
    (args_t {"kscreen-doctor", "output.HDMI-A-2.enable", "output.HDMI-A-2.priority.1", "output.DP-1.priority.2"})
  );

  // No primary configured: nothing else is touched.
  EXPECT_EQ(
    virtual_display::kscreen_enable_args("HDMI-A-2", "1280x720@120", ""),
    (args_t {"kscreen-doctor", "output.HDMI-A-2.enable", "output.HDMI-A-2.mode.1280x720@120", "output.HDMI-A-2.priority.1"})
  );
}

TEST(VirtualDisplayTests, FailedOrStillActiveTeardownRetainsRecoveryAuthority) {
  EXPECT_TRUE(virtual_display::teardown_is_verified(true, false));
  EXPECT_FALSE(virtual_display::teardown_is_verified(false, false));
  EXPECT_FALSE(virtual_display::teardown_is_verified(true, true));
  EXPECT_FALSE(virtual_display::teardown_is_verified(false, true));
}

TEST(VirtualDisplayTests, OnlyNamedWaylandCreationIsAdvertised) {
  EXPECT_TRUE(virtual_display::wayland_compositor_supports_exact_output_creation("hyprland"));
  EXPECT_FALSE(virtual_display::wayland_compositor_supports_exact_output_creation("sway"));
  EXPECT_FALSE(virtual_display::wayland_compositor_supports_exact_output_creation("kwin"));
  EXPECT_FALSE(virtual_display::wayland_compositor_supports_exact_output_creation(""));
}

TEST(VirtualDisplayTests, EvdiConnectorIdentityMustBeDiscovered) {
  EXPECT_FALSE(virtual_display::evdi_output_name_is_proven(""));
  EXPECT_TRUE(virtual_display::evdi_output_name_is_proven("DVI-I-1"));
}

TEST(VirtualDisplayTests, CreationMutexSerializesIndependentCallers) {
  std::atomic<int> active {0};
  std::atomic<int> maximum_active {0};
  std::atomic<int> completed {0};

  const auto callback = [&] {
    const int now = active.fetch_add(1) + 1;
    int observed = maximum_active.load();
    while (observed < now && !maximum_active.compare_exchange_weak(observed, now)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    active.fetch_sub(1);
    completed.fetch_add(1);
  };

  std::thread first {[&] {
    virtual_display::with_creation_lock_for_tests(callback);
  }};
  std::thread second {[&] {
    virtual_display::with_creation_lock_for_tests(callback);
  }};
  first.join();
  second.join();

  EXPECT_EQ(completed.load(), 2);
  EXPECT_EQ(maximum_active.load(), 1);
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

TEST(VirtualDisplayTests, InstalledKscreenIsDetectedBeforeItsConnectorIsConfigured) {
  using virtual_display::backend_e;
  EXPECT_EQ(virtual_display::select_preferred_backend(false, false, true), backend_e::KSCREEN_DOCTOR);
  EXPECT_FALSE(virtual_display::backend_has_required_configuration(backend_e::KSCREEN_DOCTOR, ""));
  EXPECT_TRUE(virtual_display::backend_has_required_configuration(backend_e::KSCREEN_DOCTOR, "HDMI-A-2"));

  EXPECT_EQ(virtual_display::select_preferred_backend(false, true, true), backend_e::WAYLAND_WLR);
  EXPECT_EQ(virtual_display::select_preferred_backend(true, true, true), backend_e::EVDI);
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

  EXPECT_EQ(
    virtual_display::hyprland_monitors_contain_output(monitors, "POLARIS-HEADLESS-4242-0"),
    std::optional<bool> {true}
  );
  EXPECT_EQ(
    virtual_display::hyprland_monitors_contain_output(monitors, "POLARIS-HEADLESS-4242-1"),
    std::optional<bool> {false}
  );
  EXPECT_EQ(
    virtual_display::hyprland_monitors_contain_output(monitors, "POLARIS-HEADLESS-9999-0"),
    std::optional<bool> {false}
  );
  EXPECT_FALSE(virtual_display::hyprland_monitors_contain_output(
    "not json",
    "POLARIS-HEADLESS-4242-0"
  ).has_value());
  EXPECT_FALSE(virtual_display::hyprland_monitors_contain_output(
    R"json([{"description":"missing name"}])json",
    "POLARIS-HEADLESS-4242-0"
  ).has_value());
}

TEST(VirtualDisplayTests, EvdiConnectorStatusRequiresPositiveAbsenceProof) {
  EXPECT_EQ(
    virtual_display::evdi_connector_status_is_connected("connected"),
    std::optional<bool> {true}
  );
  EXPECT_EQ(
    virtual_display::evdi_connector_status_is_connected("disconnected"),
    std::optional<bool> {false}
  );
  EXPECT_FALSE(virtual_display::evdi_connector_status_is_connected("").has_value());
  EXPECT_FALSE(virtual_display::evdi_connector_status_is_connected("unknown").has_value());
}

TEST(VirtualDisplayTests, PersistedStateTracksEveryConcurrentDisplay) {
  // A streaming session and the web UI each own a display in one process, so
  // both have to survive a round trip through the state file.
  constexpr auto state = R"json({
    "displays": [
      {"pid":4242,"output_name":"POLARIS-HEADLESS-4242-0","width":1920,"height":1080,
       "fps":60,"active":true,"backend":"wayland_wlr","device_path":""},
      {"pid":4242,"output_name":"HDMI-A-1","width":2560,"height":1440,
       "fps":120,"active":true,"backend":"kscreen_doctor","device_path":"",
       "kscreen_output_before":{"name":"HDMI-A-1","enabled":false,"current_mode_id":"12","priority":0},
       "kscreen_primary_before":{"name":"DP-1","enabled":true,"current_mode_id":"7","priority":1}}
    ]
  })json";

  const auto displays = virtual_display::parse_persisted_displays(state);
  ASSERT_EQ(displays.size(), 2u);
  EXPECT_EQ(displays[0].owner_pid, 4242);
  EXPECT_EQ(displays[0].display.output_name, "POLARIS-HEADLESS-4242-0");
  EXPECT_EQ(displays[0].display.width, 1920);
  EXPECT_EQ(displays[0].display.backend, virtual_display::backend_e::WAYLAND_WLR);
  EXPECT_EQ(displays[1].display.output_name, "HDMI-A-1");
  EXPECT_EQ(displays[1].display.fps, 120);
  ASSERT_TRUE(displays[1].display.kscreen_output_before.has_value());
  EXPECT_FALSE(displays[1].display.kscreen_output_before->enabled);
  EXPECT_EQ(displays[1].display.kscreen_output_before->current_mode_id, "12");
  ASSERT_TRUE(displays[1].display.kscreen_primary_before.has_value());
  EXPECT_EQ(displays[1].display.kscreen_primary_before->priority, 1);
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

TEST(VirtualDisplayKwinTests, BackendPreferenceReadsEveryConfigValue) {
  using virtual_display::backend_preference_e;
  using virtual_display::parse_backend_preference;
  EXPECT_EQ(parse_backend_preference(""), backend_preference_e::AUTO);
  EXPECT_EQ(parse_backend_preference("auto"), backend_preference_e::AUTO);
  EXPECT_EQ(parse_backend_preference("evdi"), backend_preference_e::EVDI);
  EXPECT_EQ(parse_backend_preference("kwin"), backend_preference_e::KWIN);
  EXPECT_EQ(parse_backend_preference("wlr"), backend_preference_e::WLR);
  EXPECT_EQ(parse_backend_preference("kscreen"), backend_preference_e::KSCREEN);
  EXPECT_FALSE(parse_backend_preference("KWin").has_value());
  EXPECT_FALSE(parse_backend_preference("krfb").has_value());

  for (const auto preference : {backend_preference_e::AUTO, backend_preference_e::EVDI, backend_preference_e::KWIN,
                                backend_preference_e::WLR, backend_preference_e::KSCREEN}) {
    EXPECT_EQ(parse_backend_preference(virtual_display::backend_preference_name(preference)), preference);
  }
}

TEST(VirtualDisplayKwinTests, AutomaticPrefersANewScreenOverABorrowedConnector) {
  using virtual_display::backend_e;
  using virtual_display::backend_preference_e;
  using virtual_display::select_backend;
  constexpr auto automatic = backend_preference_e::AUTO;

  // On Plasma the KWin screen wins even with EVDI loaded: an EVDI screen there kept
  // a stored 1.35 scale and the game opened on the primary monitor, not the stream.
  EXPECT_EQ(select_backend(automatic, {.evdi = true, .kwin = true, .wlr = true, .kscreen = true}), backend_e::KWIN_VIRTUAL_OUTPUT);
  // Off Plasma (GNOME, a wlroots desktop) the KWin probe is false and EVDI leads.
  EXPECT_EQ(select_backend(automatic, {.evdi = true, .kwin = false, .wlr = true, .kscreen = true}), backend_e::EVDI);
  EXPECT_EQ(select_backend(automatic, {.evdi = false, .kwin = true, .wlr = true, .kscreen = true}), backend_e::KWIN_VIRTUAL_OUTPUT);
  EXPECT_EQ(select_backend(automatic, {.evdi = false, .kwin = false, .wlr = true, .kscreen = true}), backend_e::WAYLAND_WLR);
  // pollux78's host in #633: KDE, no EVDI, only real monitors to borrow.
  EXPECT_EQ(select_backend(automatic, {.evdi = false, .kwin = true, .wlr = false, .kscreen = true}), backend_e::KWIN_VIRTUAL_OUTPUT);
  EXPECT_EQ(select_backend(automatic, {.evdi = false, .kwin = false, .wlr = false, .kscreen = true}), backend_e::KSCREEN_DOCTOR);
  EXPECT_EQ(select_backend(automatic, {}), backend_e::NONE);

  // The older three-argument form keeps its order, with KWin never probed.
  EXPECT_EQ(virtual_display::select_preferred_backend(false, false, true), backend_e::KSCREEN_DOCTOR);
}

TEST(VirtualDisplayKwinTests, AChosenBackendNeverFallsBackToAnother) {
  using virtual_display::backend_e;
  using virtual_display::backend_preference_e;
  using virtual_display::select_backend;

  // Set to KWin on a host where only kscreen-doctor works: borrowing a monitor
  // would do exactly what the setting was chosen to avoid.
  EXPECT_EQ(select_backend(backend_preference_e::KWIN, {.evdi = true, .kwin = false, .wlr = true, .kscreen = true}), backend_e::NONE);
  EXPECT_EQ(select_backend(backend_preference_e::KWIN, {.evdi = true, .kwin = true, .wlr = false, .kscreen = true}), backend_e::KWIN_VIRTUAL_OUTPUT);
  EXPECT_EQ(select_backend(backend_preference_e::EVDI, {.evdi = false, .kwin = true, .wlr = false, .kscreen = true}), backend_e::NONE);
  EXPECT_EQ(select_backend(backend_preference_e::KSCREEN, {.evdi = true, .kwin = true, .wlr = false, .kscreen = true}), backend_e::KSCREEN_DOCTOR);
  EXPECT_EQ(select_backend(backend_preference_e::WLR, {.evdi = true, .kwin = true, .wlr = false, .kscreen = true}), backend_e::NONE);

  const auto reason = virtual_display::forced_backend_unavailable_reason(
    backend_preference_e::KWIN,
    "KWin offers screencast version 3, and creating a screen needs version 4 (KDE Plasma 6)"
  );
  EXPECT_NE(reason.find("linux_virtual_display_backend is set to kwin"), std::string::npos);
  EXPECT_NE(reason.find("version 3"), std::string::npos);
  EXPECT_NE(reason.find("auto"), std::string::npos);
  EXPECT_EQ(virtual_display::forced_backend_unavailable_reason(backend_preference_e::AUTO, "anything"), "");
}

TEST(VirtualDisplayKwinTests, KwinScreenNeedsNoExtraConfiguration) {
  using virtual_display::backend_e;
  EXPECT_TRUE(virtual_display::backend_has_required_configuration(backend_e::KWIN_VIRTUAL_OUTPUT, ""));
  EXPECT_EQ(virtual_display::unavailable_reason_for(backend_e::KWIN_VIRTUAL_OUTPUT, false, false), "");
  EXPECT_STREQ(virtual_display::backend_name(backend_e::KWIN_VIRTUAL_OUTPUT), "KWin virtual output");
  // Still not the Hyprland backend: KWin names its outputs itself.
  EXPECT_FALSE(virtual_display::wayland_compositor_supports_exact_output_creation("kwin"));
}

TEST(VirtualDisplayKwinTests, OutputNamesAreSlotScopedAndRecognisable) {
  EXPECT_EQ(virtual_display::kwin_output_request_name(0), "polaris-0");
  EXPECT_EQ(virtual_display::kwin_output_expected_name("polaris-0"), "Virtual-polaris-0");

  EXPECT_TRUE(virtual_display::kwin_output_is_polaris_owned("Virtual-polaris-0"));
  EXPECT_TRUE(virtual_display::kwin_output_is_polaris_owned("Virtual-polaris-7"));
  EXPECT_FALSE(virtual_display::kwin_output_is_polaris_owned("Virtual-polaris-"));
  EXPECT_FALSE(virtual_display::kwin_output_is_polaris_owned("Virtual-polaris-1a"));
  // The screen-sharing portal's own virtual output, whose stored layout KWin
  // once applied to a new Polaris screen.
  EXPECT_FALSE(virtual_display::kwin_output_is_polaris_owned("Virtual-virtual-xdp-kde-"));
  EXPECT_FALSE(virtual_display::kwin_output_is_polaris_owned("DP-2"));

  EXPECT_FALSE(virtual_display::kwin_screencast_version_supported(3));
  EXPECT_TRUE(virtual_display::kwin_screencast_version_supported(4));
  EXPECT_TRUE(virtual_display::kwin_screencast_version_supported(6));
}

namespace {
  // kscreen-doctor --json on pc-papi (KWin 6.7.5) just after KWin created a
  // screen: it applied another virtual output's stored layout, so the new
  // screen came up primary at scale 0.5 on top of the real monitor.
  constexpr std::string_view spike_layout_after_create = R"({"outputs":[
    {"name":"DP-2","enabled":true,"priority":0,"scale":1,"pos":{"x":0,"y":0},"currentModeId":"2",
     "modes":[{"id":"1","name":"3840x2160@60","refreshRate":60.0,"size":{"width":3840,"height":2160}},
              {"id":"2","name":"7680x2160@60","refreshRate":59.98699951171875,"size":{"width":7680,"height":2160}}]},
    {"name":"Virtual-polaris-0","enabled":true,"priority":1,"scale":0.5,"pos":{"x":0,"y":0},"currentModeId":"1",
     "modes":[{"id":"1","name":"1920x1080@60","refreshRate":60.0,"size":{"width":1920,"height":1080}}]}
  ]})";
  constexpr std::string_view spike_layout_before_create = R"({"outputs":[
    {"name":"DP-2","enabled":true,"priority":1,"scale":1,"pos":{"x":0,"y":0},"currentModeId":"2",
     "modes":[{"id":"2","name":"7680x2160@60","refreshRate":59.98699951171875,"size":{"width":7680,"height":2160}}]}
  ]})";
}  // namespace

TEST(VirtualDisplayKwinTests, ReadsTheLayoutKscreenDoctorReports) {
  const auto layout = virtual_display::kscreen_layout_from_json(spike_layout_after_create);
  ASSERT_TRUE(layout.has_value());
  ASSERT_EQ(layout->size(), 2U);
  const auto &monitor = layout->at(0);
  EXPECT_EQ(monitor.name, "DP-2");
  EXPECT_EQ(monitor.mode_width, 7680);
  EXPECT_EQ(monitor.mode_height, 2160);
  EXPECT_NEAR(monitor.refresh_hz, 59.987, 0.001);
  EXPECT_EQ(monitor.mode_names, (std::vector<std::string> {"3840x2160@60", "7680x2160@60"}));
  const auto &screen = layout->at(1);
  EXPECT_DOUBLE_EQ(screen.scale, 0.5);
  EXPECT_EQ(screen.priority, 1);

  // Read after KWin created the screen, the primary is already the new one,
  // which is why Polaris reads it first.
  EXPECT_EQ(virtual_display::kscreen_primary_output(*layout), "Virtual-polaris-0");
  const auto before = virtual_display::kscreen_layout_from_json(spike_layout_before_create);
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(virtual_display::kscreen_primary_output(*before), "DP-2");

  // Beside the real monitor, not on top of it.
  EXPECT_EQ(virtual_display::kscreen_right_edge(*layout, "Virtual-polaris-0"), 7680);
}

TEST(VirtualDisplayKwinTests, RightEdgeUsesLogicalWidth) {
  const auto layout = virtual_display::kscreen_layout_from_json(R"({"outputs":[
    {"name":"eDP-1","enabled":true,"priority":1,"scale":2,"pos":{"x":0,"y":0},"currentModeId":"a",
     "modes":[{"id":"a","name":"3840x2160@60","refreshRate":60,"size":{"width":3840,"height":2160}}]},
    {"name":"HDMI-A-1","enabled":false,"priority":0,"scale":1,"pos":{"x":9000,"y":0},"currentModeId":"b",
     "modes":[{"id":"b","name":"1920x1080@60","refreshRate":60,"size":{"width":1920,"height":1080}}]}
  ]})");
  ASSERT_TRUE(layout.has_value());
  // 3840 device pixels at scale 2 are 1920 logical; a disabled output takes no room.
  EXPECT_EQ(virtual_display::kscreen_right_edge(*layout, "Virtual-polaris-0"), 1920);
}

TEST(VirtualDisplayKwinTests, OddKscreenAnswersNeverThrow) {
  EXPECT_FALSE(virtual_display::kscreen_layout_from_json("not json").has_value());
  EXPECT_FALSE(virtual_display::kscreen_layout_from_json(R"({"outputs":3})").has_value());
  EXPECT_FALSE(virtual_display::kscreen_layout_from_json(R"({"outputs":[{"enabled":true}]})").has_value());

  // Wrong types read as absent rather than throwing.
  const auto layout = virtual_display::kscreen_layout_from_json(R"({"outputs":[
    {"name":"DP-1","enabled":"yes","priority":"1","scale":"1","pos":{"x":"a","y":null},"currentModeId":1,
     "modes":[{"id":1},"junk",{"id":"1","size":{"width":"wide"}}]}
  ]})");
  ASSERT_TRUE(layout.has_value());
  ASSERT_EQ(layout->size(), 1U);
  EXPECT_FALSE(layout->at(0).enabled);
  EXPECT_EQ(layout->at(0).priority, 0);
  EXPECT_EQ(layout->at(0).mode_width, 0);
  EXPECT_FALSE(virtual_display::kscreen_primary_output(*layout).has_value());
}

TEST(VirtualDisplayKwinTests, KscreenArgumentsForModeAndPlacement) {
  using args_t = std::vector<std::string>;
  EXPECT_EQ(
    virtual_display::kwin_custom_mode_args("Virtual-polaris-0", 2560, 1440, 120),
    (args_t {"output.Virtual-polaris-0.addCustomMode.2560.1440.120000.full"})
  );
  EXPECT_EQ(
    virtual_display::kwin_mode_args("Virtual-polaris-0", 2560, 1440, 120),
    (args_t {"output.Virtual-polaris-0.mode.2560x1440@120"})
  );
  EXPECT_EQ(
    virtual_display::kwin_placement_args("Virtual-polaris-0", 7680, 1.0),
    (args_t {"output.Virtual-polaris-0.scale.1", "output.Virtual-polaris-0.position.7680,0"})
  );
  // The whole point of the scale: 2560x1600 of pixels laid out as 1280x800 of desktop, which is
  // what a ten inch panel needs before the thing on it can be read at arm's length.
  EXPECT_EQ(
    virtual_display::kwin_placement_args("Virtual-polaris-0", 7680, 2.0),
    (args_t {"output.Virtual-polaris-0.scale.2", "output.Virtual-polaris-0.position.7680,0"})
  );
  // A fractional scale keeps its point and never a comma: kscreen-doctor rejects 1,5, and a host
  // started under a locale that writes decimals that way would otherwise fail only there.
  EXPECT_EQ(virtual_display::kwin_scale_value(1.5), "1.5");
  EXPECT_EQ(virtual_display::kwin_scale_value(2.0), "2");
  // Nobody said, so the screen is made the way every earlier release made it.
  EXPECT_EQ(virtual_display::kwin_scale_value(0.0), "1");
}

namespace {
  virtual_display::kscreen_output_layout_t ranked_screen(std::string name, int priority, bool enabled = true) {
    virtual_display::kscreen_output_layout_t output;
    output.name = std::move(name);
    output.priority = priority;
    output.enabled = enabled;
    return output;
  }
}  // namespace

TEST(VirtualDisplayKwinTests, StreamScreenIsRankedAfterEveryScreenInTheirOldOrder) {
  using args_t = std::vector<std::string>;
  // Plasma gives each rank its own desktop and panel, so every screen keeps its
  // rank and the stream screen comes after all of them. Three monitors, listed
  // out of rank order, one disabled, and KWin's stored entry for the stream
  // screen ranked first.
  const std::vector<virtual_display::kscreen_output_layout_t> three_monitors {
    ranked_screen("DP-1", 3),
    ranked_screen("HDMI-A-1", 1),
    ranked_screen("DP-3", 0, false),
    ranked_screen("Virtual-polaris-0", 1),
    ranked_screen("DP-2", 2),
  };
  EXPECT_EQ(
    virtual_display::kwin_priority_args("Virtual-polaris-0", three_monitors),
    (args_t {
      "output.HDMI-A-1.priority.1",
      "output.DP-2.priority.2",
      "output.DP-1.priority.3",
      "output.Virtual-polaris-0.priority.4",
    })
  );
  EXPECT_EQ(
    virtual_display::kwin_priority_args("Virtual-polaris-0", {ranked_screen("DP-2", 1)}),
    (args_t {"output.DP-2.priority.1", "output.Virtual-polaris-0.priority.2"})
  );
  // KWin 6.7.5 reports a lone monitor with priority 0. Leaving it out would
  // rank the stream screen first, and Plasma would move the desktop onto it.
  EXPECT_EQ(
    virtual_display::kwin_priority_args("Virtual-polaris-0", {ranked_screen("DP-2", 0)}),
    (args_t {"output.DP-2.priority.1", "output.Virtual-polaris-0.priority.2"})
  );
  EXPECT_EQ(
    virtual_display::kwin_priority_args("Virtual-polaris-0", {ranked_screen("HDMI-A-1", 0), ranked_screen("DP-2", 1)}),
    (args_t {"output.DP-2.priority.1", "output.HDMI-A-1.priority.2", "output.Virtual-polaris-0.priority.3"})
  );
  EXPECT_EQ(
    virtual_display::kwin_priority_args("Virtual-polaris-0", {}),
    (args_t {"output.Virtual-polaris-0.priority.1"})
  );
}

TEST(VirtualDisplayKwinTests, RankingCheckCatchesAStreamScreenAboveAMonitor) {
  const std::vector<virtual_display::kscreen_output_layout_t> before {
    ranked_screen("DP-2", 1),
    ranked_screen("HDMI-A-1", 2),
  };
  EXPECT_TRUE(virtual_display::kwin_ranking_matches(
    {ranked_screen("Virtual-polaris-0", 3), ranked_screen("HDMI-A-1", 2), ranked_screen("DP-2", 1)},
    before,
    "Virtual-polaris-0"
  ));
  // The primary kept first is not enough: the second monitor's desktop and panel
  // would move onto the stream screen.
  EXPECT_FALSE(virtual_display::kwin_ranking_matches(
    {ranked_screen("DP-2", 1), ranked_screen("Virtual-polaris-0", 2), ranked_screen("HDMI-A-1", 3)},
    before,
    "Virtual-polaris-0"
  ));
  EXPECT_FALSE(virtual_display::kwin_ranking_matches(
    {ranked_screen("Virtual-polaris-0", 1), ranked_screen("DP-2", 2), ranked_screen("HDMI-A-1", 3)},
    before,
    "Virtual-polaris-0"
  ));
  EXPECT_FALSE(virtual_display::kwin_ranking_matches(
    {ranked_screen("HDMI-A-1", 1), ranked_screen("DP-2", 2), ranked_screen("Virtual-polaris-0", 3)},
    before,
    "Virtual-polaris-0"
  ));
  // The lone monitor KWin reported at priority 0 counts as first.
  EXPECT_TRUE(virtual_display::kwin_ranking_matches(
    {ranked_screen("DP-2", 1), ranked_screen("Virtual-polaris-0", 2)},
    {ranked_screen("DP-2", 0)},
    "Virtual-polaris-0"
  ));
  EXPECT_FALSE(virtual_display::kwin_ranking_matches(
    {ranked_screen("Virtual-polaris-0", 1), ranked_screen("DP-2", 0)},
    {ranked_screen("DP-2", 0)},
    "Virtual-polaris-0"
  ));
}

namespace {
  virtual_display::kscreen_output_layout_t placed_screen(std::string name, int x, int y, int width, bool enabled = true) {
    virtual_display::kscreen_output_layout_t output;
    output.name = std::move(name);
    output.enabled = enabled;
    output.x = x;
    output.y = y;
    output.mode_width = width;
    output.mode_height = 2160;
    return output;
  }
}  // namespace

TEST(VirtualDisplayKwinTests, OtherScreensGoBackWhereTheyWere) {
  using args_t = std::vector<std::string>;
  // pc-papi: DP-2 at 0,0 before the stream. KWin then applied the layout it had
  // stored for "DP-2 plus a Polaris screen", which put DP-2 at 1024,0 and the
  // screen at 8704,0, and the monitor stayed moved for the whole stream.
  const std::vector<virtual_display::kscreen_output_layout_t> before {
    placed_screen("DP-2", 0, 0, 7680),
    placed_screen("HDMI-A-1", 7680, -200, 1920),
    placed_screen("DP-3", 9000, 0, 1920, false),
  };
  EXPECT_EQ(
    virtual_display::kwin_keep_positions_args("Virtual-polaris-0", before),
    (args_t {"output.DP-2.position.0,0", "output.HDMI-A-1.position.7680,-200"})
  );
  // The new screen goes past them as they were, not as KWin's stored layout had them.
  EXPECT_EQ(virtual_display::kscreen_right_edge(before, "Virtual-polaris-0"), 9600);

  const std::vector<virtual_display::kscreen_output_layout_t> shifted {
    placed_screen("DP-2", 1024, 0, 7680),
    placed_screen("HDMI-A-1", 7680, -200, 1920),
    placed_screen("Virtual-polaris-0", 8704, 0, 1920),
  };
  EXPECT_FALSE(virtual_display::kwin_positions_match(shifted, before, "Virtual-polaris-0"));
  const std::vector<virtual_display::kscreen_output_layout_t> restored {
    placed_screen("DP-2", 0, 0, 7680),
    placed_screen("HDMI-A-1", 7680, -200, 1920),
    placed_screen("Virtual-polaris-0", 9600, 0, 1920),
  };
  EXPECT_TRUE(virtual_display::kwin_positions_match(restored, before, "Virtual-polaris-0"));
  // A monitor that went missing or dark is not where it was.
  EXPECT_FALSE(virtual_display::kwin_positions_match({restored[0], restored[2]}, before, "Virtual-polaris-0"));
  // A screen listed before under the stream screen's own name is left to the placement.
  EXPECT_TRUE(virtual_display::kwin_keep_positions_args("DP-2", {placed_screen("DP-2", 0, 0, 7680)}).empty());
}

TEST(VirtualDisplayKwinTests, OnlyTouchAndPenFollowTheStreamScreen) {
  // A tap and a pen stroke land on a point of the screen, and KWin spread both over
  // every monitor until they were tied to the stream screen.
  EXPECT_TRUE(virtual_display::routes_to_stream_screen("Touch passthrough"));
  EXPECT_TRUE(virtual_display::routes_to_stream_screen("Pen passthrough"));
  // KWin places an absolute pointer over the whole workspace whatever its outputName
  // says, so tying it would change nothing and report a tie that does not hold.
  EXPECT_FALSE(virtual_display::routes_to_stream_screen("Polaris Mouse passthrough (absolute)"));
  // The relative mouse moves the cursor wherever it is, and a keyboard follows the focus.
  EXPECT_FALSE(virtual_display::routes_to_stream_screen("Polaris Mouse passthrough"));
  EXPECT_FALSE(virtual_display::routes_to_stream_screen("Polaris Keyboard passthrough"));
  EXPECT_FALSE(virtual_display::routes_to_stream_screen("Logitech USB Receiver Mouse"));
  EXPECT_FALSE(virtual_display::routes_to_stream_screen(""));

  EXPECT_EQ(virtual_display::input_event_name("/dev/input/event31"), "event31");
  EXPECT_EQ(virtual_display::input_event_name("event7"), "event7");
  // Anything else never becomes part of a KWin object path.
  EXPECT_EQ(virtual_display::input_event_name("/dev/input/js0"), "");
  EXPECT_EQ(virtual_display::input_event_name("/dev/input/event"), "");
  EXPECT_EQ(virtual_display::input_event_name("/dev/input/event3/../../x"), "");
  EXPECT_EQ(virtual_display::input_event_name("/dev/input/event3x"), "");
  EXPECT_EQ(virtual_display::input_event_name(""), "");
}

TEST(VirtualDisplayKwinTests, ModeAndPlacementReadback) {
  virtual_display::kscreen_output_layout_t screen;
  screen.name = "Virtual-polaris-0";
  screen.enabled = true;
  screen.mode_width = 1920;
  screen.mode_height = 1080;
  screen.refresh_hz = 119.93;  // What KWin ran for a 120 Hz custom mode on the test host.
  EXPECT_TRUE(virtual_display::kwin_mode_matches(screen, 1920, 1080, 120));
  EXPECT_FALSE(virtual_display::kwin_mode_matches(screen, 1920, 1080, 60));
  EXPECT_FALSE(virtual_display::kwin_mode_matches(screen, 2560, 1440, 120));
  screen.refresh_hz = 0.0;
  EXPECT_FALSE(virtual_display::kwin_mode_matches(screen, 1920, 1080, 120));

  screen.scale = 1.0;
  screen.x = 7680;
  screen.y = 0;
  screen.priority = 2;
  EXPECT_TRUE(virtual_display::kwin_placement_matches(screen, 7680, 1.0));
  EXPECT_FALSE(virtual_display::kwin_placement_matches(screen, 0, 1.0));
  screen.scale = 0.5;
  EXPECT_FALSE(virtual_display::kwin_placement_matches(screen, 7680, 1.0));

  // The check is against the scale that was asked for, not against 1. Verifying a scaled screen
  // by the old rule would report every one of them as a failed placement.
  screen.scale = 2.0;
  EXPECT_TRUE(virtual_display::kwin_placement_matches(screen, 7680, 2.0));
  EXPECT_FALSE(virtual_display::kwin_placement_matches(screen, 7680, 1.0));
  EXPECT_FALSE(virtual_display::kwin_placement_matches(screen, 7680, 1.5));
}

TEST(VirtualDisplayKwinTests, WindowScriptMovesApplicationWindowsOntoTheScreen) {
  // The whole script, because nothing in CI runs it. KWin 6.7.5 evaluated and
  // kept this exact text, and the same script without the host prompt list
  // moved an XWayland and a Wayland window onto the stream screen there.
  // Change it only with a check on a live KWin.
  //  - Only application windows, dialogs and splash screens move: never the
  //    desktop, panels, notifications or popups.
  //  - The desktop's own prompts stay with whoever sits at the host.
  //  - A window already on another Polaris screen stays there.
  //  - An application window or dialog on the stream screen takes the focus:
  //    KWin kept it at the desk, and Control on pc-papi then ignored the
  //    Retroid's controller and taps until it was activated by hand.
  EXPECT_EQ(virtual_display::kwin_window_follow_script("Virtual-polaris-0"), R"JS(// Polaris: moves windows onto its Host Virtual Display screen while that screen exists.
const target = "Virtual-polaris-0";
// The desktop's own prompts are for whoever sits at the host.
const hostPrompts = ["polkit-kde-authentication-agent-1", "org.kde.polkit-kde-authentication-agent-1",
  "ksshaskpass", "org.kde.ksshaskpass", "kwalletd5", "org.kde.kwalletd5", "kwalletd6", "org.kde.kwalletd6",
  "krunner", "org.kde.krunner", "plasmashell", "org.kde.plasmashell"];
function outputNamed(name) {
  const screens = workspace.screens;
  for (let i = 0; i < screens.length; i++) { if (screens[i].name === name) return screens[i]; }
  return null;
}
function isHostPrompt(window) {
  return hostPrompts.indexOf(String(window.resourceClass || "")) >= 0 ||
         hostPrompts.indexOf(String(window.desktopFileName || "")) >= 0;
}
workspace.windowAdded.connect(function (window) {
  if (!window || !(window.normalWindow || window.dialog || window.splash)) return;
  if (isHostPrompt(window)) return;
  const screen = outputNamed(target);
  if (!screen) return;
  if (window.output !== screen) {
    if (window.output && window.output.name.indexOf("Virtual-polaris-") === 0) return;
    workspace.sendClientToScreen(window, screen);
  }
  // The player is at the stream, so the window they started takes the focus too. KWin
  // otherwise leaves it at the desk, and an unfocused game ignores its controller and taps.
  if (window.normalWindow || window.dialog) workspace.activeWindow = window;
});
)JS");
  EXPECT_EQ(virtual_display::kwin_window_follow_plugin_name("Virtual-polaris-0"), "polaris-follow-Virtual-polaris-0");

  // The name is a JSON string literal, so a quote cannot end it early.
  const auto hostile = virtual_display::kwin_window_follow_script("x\"; evil(); \"");
  EXPECT_NE(hostile.find(R"(const target = "x\"; evil(); \"";)"), std::string::npos);
}

TEST(VirtualDisplayKwinTests, NamesWhyKwinCannotIdentifyPolaris) {
  // Dumpable: KWin can read /proc/<pid>/exe, so the cause is elsewhere.
  EXPECT_TRUE(virtual_display::kwin_unidentifiable_process_reason(1, true).empty());
  // 0 by default, 2 under fs.suid_dumpable=2 (apport, some systemd-coredump setups).
  for (int dumpable : {0, 2}) {
    const auto held = virtual_display::kwin_unidentifiable_process_reason(dumpable, true);
    EXPECT_NE(held.find("--enable-kms"), std::string::npos) << dumpable;
    EXPECT_NE(held.find("leave capture on auto or portal"), std::string::npos) << dumpable;
    // Non-dumpable without a capability: say so rather than blame one it does not hold.
    const auto none = virtual_display::kwin_unidentifiable_process_reason(dumpable, false);
    EXPECT_NE(none.find("NoNewPrivileges"), std::string::npos) << dumpable;
    EXPECT_EQ(none.find("holds file capabilities"), std::string::npos) << dumpable;
  }
}

TEST(VirtualDisplayKwinTests, PersistedKwinScreenRoundTrips) {
  const auto entries = virtual_display::parse_persisted_displays(R"({"displays":[
    {"pid":4242,"output_name":"Virtual-polaris-0","width":1920,"height":1080,"fps":120,"active":true,
     "backend":"kwin_virtual_output",
     "kscreen_primary_before":{"name":"DP-2","enabled":true,"current_mode_id":"2","priority":1}}
  ]})");
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries.front().display.backend, virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT);
  EXPECT_EQ(entries.front().display.output_name, "Virtual-polaris-0");
  ASSERT_TRUE(entries.front().display.kscreen_primary_before.has_value());
  EXPECT_EQ(entries.front().display.kscreen_primary_before->name, "DP-2");
}

#else
TEST(VirtualDisplayTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only virtual display tests";
}
#endif
