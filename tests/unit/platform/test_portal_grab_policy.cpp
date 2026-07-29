/**
 * @file tests/unit/platform/test_portal_grab_policy.cpp
 * @brief Test XDG Desktop Portal source selection policy.
 */

#include "../../tests_common.h"
#include "src/platform/common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef POLARIS_BUILD_PORTAL
namespace portal {
  std::uint32_t capture_type_for_stream_display_for_tests(bool headless_mode, bool use_cage_compositor);
  void install_shutdown_probe_for_tests();
  void cleanup_shutdown_probe_for_tests();
  bool global_capture_present_for_tests();
  bool global_session_present_for_tests();
  bool capture_running_when_loop_stopped_for_tests();
  unsigned teardown_state_callback_count_for_tests();
  std::vector<std::string> shutdown_events_for_tests();
  void exercise_state_callback_log_failure_for_tests();
  void exercise_process_callback_failure_for_tests();
}

namespace platf {
  std::unique_ptr<deinit_t> linux_deinit_guard_for_tests();
}

TEST(PortalGrabPolicyTests, DesktopDisplayRequestsMonitorSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(false, false), 1u);
}

TEST(PortalGrabPolicyTests, PrivateAndWindowedCagePathsRequestWindowSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(true, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(false, true), 2u);
}

TEST(PortalPipeWireShutdownTests, ActiveGlobalCaptureQuiescesCallbacksBeforeLoggingLifetimeEnds) {
  portal::install_shutdown_probe_for_tests();

  auto deinit_guard = platf::linux_deinit_guard_for_tests();
  deinit_guard.reset();

  EXPECT_FALSE(portal::global_capture_present_for_tests());
  EXPECT_FALSE(portal::global_session_present_for_tests());
  EXPECT_EQ(portal::shutdown_events_for_tests(),
    (std::vector<std::string> {"loop_stop", "stream_destroy", "loop_destroy"}));
  EXPECT_FALSE(portal::capture_running_when_loop_stopped_for_tests());
  EXPECT_EQ(portal::teardown_state_callback_count_for_tests(), 0u);

  auto second_deinit_guard = platf::linux_deinit_guard_for_tests();
  second_deinit_guard.reset();
  EXPECT_EQ(portal::shutdown_events_for_tests(),
    (std::vector<std::string> {"loop_stop", "stream_destroy", "loop_destroy"}));

  portal::cleanup_shutdown_probe_for_tests();
}

TEST(PortalPipeWireShutdownTests, StateCallbackContainsLoggingFailure) {
  EXPECT_NO_THROW(portal::exercise_state_callback_log_failure_for_tests());
}

TEST(PortalPipeWireShutdownTests, ProcessCallbackRequeuesBufferAfterPostDequeueFailure) {
  EXPECT_NO_THROW(portal::exercise_process_callback_failure_for_tests());
}
#endif
