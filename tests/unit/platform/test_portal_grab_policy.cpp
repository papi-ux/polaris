/**
 * @file tests/unit/platform/test_portal_grab_policy.cpp
 * @brief Test XDG Desktop Portal source selection policy.
 */

#include "../../tests_common.h"

#include <cstdint>

namespace portal {
  std::uint32_t capture_type_for_stream_display_for_tests(bool headless_mode, bool use_cage_compositor);
}

TEST(PortalGrabPolicyTests, DesktopDisplayRequestsMonitorSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(false, false), 1u);
}

TEST(PortalGrabPolicyTests, PrivateAndWindowedCagePathsRequestWindowSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(true, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(false, true), 2u);
}
