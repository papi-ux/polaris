/**
 * @file tests/unit/test_session_media_gate.cpp
 * @brief Behavioral tests for Linux media transport and teardown admission policy.
 */

#include <gtest/gtest.h>

#include "src/platform/linux/pipewire_transport_policy.h"
#include "src/platform/linux/session_media_gate.h"

#include <chrono>
#include <future>
#include <optional>
#include <vector>

using namespace std::chrono_literals;

TEST(PipeWireTransportPolicyTests, DmaBufNegotiationAdvertisesOnlyDmaBufAllocation) {
  EXPECT_EQ(
    pipewire_transport::offered_buffer_transports(true),
    (std::vector<pipewire_transport::buffer_transport_e> {
      pipewire_transport::buffer_transport_e::dmabuf,
    })
  );
}

TEST(PipeWireTransportPolicyTests, CpuNegotiationAdvertisesOnlyMappedCpuAllocation) {
  EXPECT_EQ(
    pipewire_transport::offered_buffer_transports(false),
    (std::vector<pipewire_transport::buffer_transport_e> {
      pipewire_transport::buffer_transport_e::memfd,
      pipewire_transport::buffer_transport_e::memptr,
    })
  );
}

TEST(SessionMediaGateTests, NewStartWaitsForEveryTeardownOwnerToReachTerminalState) {
  session_media::teardown_gate_t gate;
  std::optional<session_media::teardown_owner_t> first_teardown {gate.begin_teardown()};
  std::optional<session_media::teardown_owner_t> final_teardown {gate.begin_teardown()};

  auto pending_start = std::async(std::launch::async, [&gate]() {
    return gate.begin_start();
  });

  EXPECT_EQ(pending_start.wait_for(20ms), std::future_status::timeout);
  first_teardown.reset();
  EXPECT_EQ(pending_start.wait_for(20ms), std::future_status::timeout);

  final_teardown.reset();
  ASSERT_EQ(pending_start.wait_for(1s), std::future_status::ready);
  auto admitted_start = pending_start.get();
  EXPECT_TRUE(admitted_start.owns_start());
  EXPECT_FALSE(gate.teardown_in_progress());
}

TEST(SessionMediaGateTests, TeardownWaitsForAnAdmittedStartBeforeTakingOwnership) {
  session_media::teardown_gate_t gate;
  std::optional<session_media::start_owner_t> admitted_start {gate.begin_start()};

  auto pending_teardown = std::async(std::launch::async, [&gate]() {
    return gate.begin_teardown();
  });

  EXPECT_EQ(pending_teardown.wait_for(20ms), std::future_status::timeout);
  EXPECT_TRUE(gate.teardown_in_progress());

  admitted_start.reset();
  ASSERT_EQ(pending_teardown.wait_for(1s), std::future_status::ready);
  auto teardown = pending_teardown.get();
  EXPECT_TRUE(teardown.owns_teardown());
}

TEST(SessionMediaGateTests, TeardownAnnouncementRunsBeforeWaitingForAdmittedStart) {
  session_media::teardown_gate_t gate;
  std::optional<session_media::start_owner_t> admitted_start {gate.begin_start()};
  std::promise<void> announced;
  auto announcement = announced.get_future();

  auto pending_teardown = std::async(std::launch::async, [&gate, &announced]() {
    return gate.begin_teardown([&announced]() {
      announced.set_value();
    });
  });

  EXPECT_EQ(announcement.wait_for(1s), std::future_status::ready)
    << "teardown cancellation must be announced before waiting for active starts";
  EXPECT_EQ(pending_teardown.wait_for(20ms), std::future_status::timeout);
  admitted_start.reset();
  ASSERT_EQ(pending_teardown.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(pending_teardown.get().owns_teardown());
}

TEST(SessionMediaGateTests, TerminalWaitDoesNotReturnWhileAnOwnedCleanupIsRunning) {
  session_media::teardown_gate_t gate;
  std::optional<session_media::teardown_owner_t> cleanup_owner {gate.begin_teardown()};

  auto terminal = std::async(std::launch::async, [&gate]() {
    gate.wait_for_idle();
  });

  EXPECT_EQ(terminal.wait_for(20ms), std::future_status::timeout);
  cleanup_owner.reset();
  EXPECT_EQ(terminal.wait_for(1s), std::future_status::ready);
}

TEST(SessionMediaGateTests, RootFenceKeepsAdmissionClosedAfterSubordinateCleanupCompletes) {
  session_media::teardown_gate_t gate;
  std::optional<session_media::teardown_owner_t> root {gate.begin_teardown()};
  std::optional<session_media::teardown_owner_t> cleanup {gate.begin_teardown()};

  auto quiescent = std::async(std::launch::async, [&gate, &root]() {
    gate.wait_for_other_teardowns(*root);
  });
  auto pending_start = std::async(std::launch::async, [&gate]() {
    return gate.begin_start();
  });

  EXPECT_EQ(quiescent.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(pending_start.wait_for(20ms), std::future_status::timeout);
  cleanup.reset();
  EXPECT_EQ(quiescent.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(pending_start.wait_for(20ms), std::future_status::timeout);
  root.reset();
  ASSERT_EQ(pending_start.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(pending_start.get().owns_start());
}

TEST(SessionMediaGateTests, OutstandingOwnerCanFinishAfterGateWrapperIsDestroyed) {
  std::optional<session_media::teardown_owner_t> teardown;
  {
    session_media::teardown_gate_t gate;
    teardown.emplace(gate.begin_teardown());
    EXPECT_TRUE(gate.teardown_in_progress());
  }

  EXPECT_TRUE(teardown->owns_teardown());
  teardown.reset();
}