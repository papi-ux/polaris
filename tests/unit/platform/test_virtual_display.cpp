/**
 * @file tests/unit/platform/test_virtual_display.cpp
 * @brief Test Linux virtual display backend detection helpers.
 */
#include "../../tests_common.h"

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
#else
TEST(VirtualDisplayTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only virtual display tests";
}
#endif
