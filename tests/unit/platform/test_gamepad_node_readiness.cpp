/**
 * @file tests/unit/platform/test_gamepad_node_readiness.cpp
 * @brief Test the bounded wait for a virtual gamepad's own input nodes.
 *
 * A fake device stands in for a UHID DualSense: its nodes appear in sysfs and /dev on a scripted
 * timeline, and the fake clock advances only when the wait sleeps.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include "src/platform/linux/input/inputtino_gamepad_readiness.h"

  #include <algorithm>
  #include <chrono>
  #include <optional>
  #include <set>
  #include <stdexcept>
  #include <string>
  #include <vector>

namespace {
  using namespace std::chrono_literals;
  using std::chrono::milliseconds;
  using platf::gamepad::readiness::backoff_t;
  using platf::gamepad::readiness::environment_t;
  using platf::gamepad::readiness::policy_t;
  using platf::gamepad::readiness::result_t;
  using platf::gamepad::readiness::status_e;
  using platf::gamepad::readiness::time_point_t;
  using platf::gamepad::readiness::wait_for_owned_nodes;

  struct fake_node_t {
    std::string path;
    milliseconds listed_at {0};  ///< When the device's sysfs enumeration starts to list it.
    milliseconds exists_at {0};  ///< When its /dev node exists.
    std::optional<milliseconds> gone_at;  ///< When it is removed again.
  };

  struct fake_device_t {
    milliseconds now {0};
    std::vector<fake_node_t> owned;
    std::vector<std::string> extra_listed;  ///< Malformed or foreign entries the enumeration also returns.
    std::set<std::string> host_nodes;  ///< Existing nodes of other devices, such as a host controller.
    milliseconds failing_until {0};
    bool clock_frozen = false;

    bool alive(const fake_node_t &node, milliseconds since) const {
      return now >= since && (!node.gone_at || now < *node.gone_at);
    }

    environment_t environment() {
      return {
        .probe = [this]() {
          if (now < failing_until) {
            throw std::runtime_error("uhid directory vanished during enumeration");
          }
          std::vector<std::string> listed = extra_listed;
          for (const auto &node : owned) {
            if (alive(node, node.listed_at)) {
              listed.push_back(node.path);
            }
          }
          return listed;
        },
        .node_exists = [this](const std::string &path) {
          if (host_nodes.contains(path)) {
            return true;
          }
          return std::any_of(owned.begin(), owned.end(), [&](const fake_node_t &node) {
            return node.path == path && alive(node, node.exists_at);
          });
        },
        .now = [this]() {
          return time_point_t {} + now;
        },
        .sleep = [this](milliseconds duration) {
          if (!clock_frozen) {
            now += duration;
          }
        },
      };
    }

    result_t wait(const policy_t &policy = {}) {
      return wait_for_owned_nodes(environment(), policy);
    }
  };

  /// hid-playstation registering a pad's gamepad (event and js), motion sensors and touchpad.
  fake_device_t dualsense_registered_at(milliseconds at) {
    fake_device_t device;
    device.owned = {
      {"/dev/input/event161", at, at},
      {"/dev/input/js5", at, at},
      {"/dev/input/event162", at + 1ms, at + 1ms},
      {"/dev/input/event163", at + 1ms, at + 1ms},
    };
    return device;
  }

  const std::vector<std::string> all_dualsense_nodes {
    "/dev/input/event161",
    "/dev/input/event162",
    "/dev/input/event163",
    "/dev/input/js5",
  };

  /// The original registration: one node list read as create() returns.
  policy_t one_shot() {
    return {.timeout = 0ms, .poll_interval = 20ms, .settle = 50ms};
  }
}  // namespace

TEST(GamepadNodeReadinessTests, TheOriginalOneShotSnapshotMissesNodesRegisteredAfterCreate) {
  auto device = dualsense_registered_at(40ms);
  const auto snapshot = device.wait(one_shot());

  EXPECT_EQ(snapshot.probes, 1);
  EXPECT_EQ(snapshot.status, status_e::empty);
  EXPECT_TRUE(snapshot.nodes.empty());
  EXPECT_FALSE(snapshot.usable());
}

TEST(GamepadNodeReadinessTests, DelayedNodesAreAllRegisteredOnceTheySettle) {
  auto device = dualsense_registered_at(40ms);
  const auto result = device.wait();

  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_TRUE(result.usable());
  EXPECT_EQ(result.nodes, all_dualsense_nodes);
  EXPECT_GE(result.waited, 40ms + policy_t {}.settle);
  EXPECT_LT(result.waited, policy_t {}.timeout);
}

TEST(GamepadNodeReadinessTests, NodesAlreadyPresentReturnAfterOnlyTheSettleTime) {
  auto device = dualsense_registered_at(0ms);
  device.owned[2].listed_at = device.owned[2].exists_at = 0ms;
  device.owned[3].listed_at = device.owned[3].exists_at = 0ms;
  const auto result = device.wait();

  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_EQ(result.nodes, all_dualsense_nodes);
  EXPECT_LE(result.waited, policy_t {}.settle + policy_t {}.poll_interval);
}

TEST(GamepadNodeReadinessTests, NodesThatNeverAppearTimeOutEmptyWithinTheBound) {
  fake_device_t device;
  const policy_t policy;
  const auto result = device.wait(policy);

  EXPECT_EQ(result.status, status_e::empty);
  EXPECT_FALSE(result.usable());
  EXPECT_TRUE(result.nodes.empty());
  EXPECT_FALSE(result.detail.empty());
  EXPECT_GE(result.waited, policy.timeout);
  EXPECT_LE(result.waited, policy.timeout + policy.poll_interval);
  EXPECT_LE(result.probes, policy.timeout / policy.poll_interval + 2);
}

TEST(GamepadNodeReadinessTests, AProbeThatKeepsFailingIsAnErrorAndAdmitsNothing) {
  auto device = dualsense_registered_at(0ms);
  device.failing_until = 1h;
  const policy_t policy;
  const auto result = device.wait(policy);

  EXPECT_EQ(result.status, status_e::error);
  EXPECT_FALSE(result.usable());
  EXPECT_TRUE(result.nodes.empty());
  EXPECT_NE(result.detail.find("vanished"), std::string::npos);
  EXPECT_LE(result.waited, policy.timeout + policy.poll_interval);
}

TEST(GamepadNodeReadinessTests, ATransientProbeErrorRecovers) {
  auto device = dualsense_registered_at(10ms);
  device.failing_until = 30ms;
  const auto result = device.wait();

  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_EQ(result.nodes, all_dualsense_nodes);
}

TEST(GamepadNodeReadinessTests, OnlyTheDevicesOwnExistingNodesAreAdmitted) {
  auto device = dualsense_registered_at(10ms);
  // Host controllers and other devices exist, but the device's own enumeration never lists them.
  device.host_nodes = {"/dev/input/event3", "/dev/input/js0", "/dev/hidraw2", "/dev/input/mouse0"};
  // Entries that are not device nodes are ignored even when listed.
  device.extra_listed = {
    "/dev/input/mouse0",
    "/dev/input/event",
    "/dev/input/event161x",
    "/sys/devices/virtual/misc/uhid/0005:054C:0CE6.0029/input/input161",
    "/dev/input/event161",
  };
  const auto result = device.wait();

  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_EQ(result.nodes, all_dualsense_nodes);
  for (const auto &host : device.host_nodes) {
    EXPECT_EQ(std::count(result.nodes.begin(), result.nodes.end(), host), 0) << host;
  }
}

TEST(GamepadNodeReadinessTests, AListedNodeJoinsOnlyOnceItsDeviceFileExists) {
  auto device = dualsense_registered_at(5ms);
  device.owned[1].exists_at = 30ms;  // js5 is in sysfs before /dev has it
  const auto result = device.wait();

  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_EQ(result.nodes, all_dualsense_nodes);
  EXPECT_GE(result.waited, 30ms + policy_t {}.settle);
}

TEST(GamepadNodeReadinessTests, ADepartingPadWithTheSameMacIsNotAdmitted) {
  auto device = dualsense_registered_at(10ms);
  // A previous pad with this MAC still enumerates until its nodes go at 20 ms, and one stale entry
  // stays listed with no device file at all.
  device.owned.push_back({"/dev/input/event150", 0ms, 0ms, 20ms});
  device.owned.push_back({"/dev/input/event149", 0ms, 1h});
  const auto result = device.wait();

  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_EQ(result.nodes, all_dualsense_nodes);
}

TEST(GamepadNodeReadinessTests, NodesThatKeepChangingEndPartialWithOnlyPresentNodes) {
  fake_device_t device;
  device.owned.push_back({"/dev/input/event161", 0ms, 0ms});
  for (auto at = 0ms; at < 3s; at += 60ms) {
    device.owned.push_back({"/dev/input/js5", at, at, at + 30ms});
  }
  const policy_t policy;
  const auto result = device.wait(policy);

  EXPECT_EQ(result.status, status_e::partial);
  EXPECT_TRUE(result.usable());
  EXPECT_NE(std::find(result.nodes.begin(), result.nodes.end(), "/dev/input/event161"), result.nodes.end());
  for (const auto &node : result.nodes) {
    EXPECT_TRUE(node == "/dev/input/event161" || node == "/dev/input/js5") << node;
  }
  EXPECT_LE(result.waited, policy.timeout + policy.poll_interval);
}

TEST(GamepadNodeReadinessTests, AClockThatNeverAdvancesStillStops) {
  fake_device_t device;
  device.clock_frozen = true;
  const policy_t policy;
  const auto result = device.wait(policy);

  EXPECT_EQ(result.status, status_e::empty);
  EXPECT_LE(result.probes, policy.timeout / policy.poll_interval + 2);
}

TEST(GamepadNodeReadinessTests, TheDefaultPolicyIsBounded) {
  const policy_t policy;

  EXPECT_GT(policy.timeout, 0ms);
  EXPECT_LE(policy.timeout, 3s);
  EXPECT_GT(policy.poll_interval, 0ms);
  EXPECT_LT(policy.poll_interval, policy.settle);
  EXPECT_LT(policy.settle, policy.timeout);
}

TEST(GamepadNodeReadinessTests, BackoffSpacesOutRetriesAndClearsOnSuccess) {
  backoff_t backoff {30s, 5min};
  const auto start = time_point_t {} + 1h;
  EXPECT_FALSE(backoff.blocks(start));

  backoff.failed(start);
  EXPECT_TRUE(backoff.blocks(start + 29s));
  EXPECT_FALSE(backoff.blocks(start + 30s));

  backoff.failed(start + 30s);
  EXPECT_TRUE(backoff.blocks(start + 89s));
  EXPECT_FALSE(backoff.blocks(start + 90s));

  for (int i = 0; i < 10; ++i) {
    backoff.failed(start);
  }
  EXPECT_EQ(backoff.retry_at(), start + 5min);

  backoff.succeeded();
  EXPECT_FALSE(backoff.blocks(start));
}

#endif
