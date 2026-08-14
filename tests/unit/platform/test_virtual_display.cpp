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
