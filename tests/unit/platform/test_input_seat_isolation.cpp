/**
 * @file tests/unit/platform/test_input_seat_isolation.cpp
 * @brief Seat isolation is only real when the kernel agrees it happened.
 */
#include "../../tests_common.h"

#ifdef __linux__

  #include "src/platform/linux/input/input_seat_isolation.h"

  #include <optional>
  #include <string>
  #include <string_view>
  #include <vector>

namespace {

  using namespace platf::input_isolation;

  TEST(InputSeatIsolationMarker, BuildsTheMarkerOnlyWhenIsolationIsEnabled) {
    EXPECT_EQ(client_input_phys_marker(true, "keyboard"), "polaris/client-input-seat-isolated/keyboard");
    EXPECT_TRUE(client_input_phys_marker(false, "keyboard").empty());
  }

  TEST(InputSeatIsolationMarker, RecognisesOnlyTheMarkerTheUdevRulesMatch) {
    EXPECT_TRUE(phys_carries_isolation_marker("polaris/client-input-seat-isolated/mouse"));
    EXPECT_FALSE(phys_carries_isolation_marker("polaris/client-gamepad-seat-isolated/xbox"));
    EXPECT_FALSE(phys_carries_isolation_marker(""));
    EXPECT_FALSE(phys_carries_isolation_marker("usb-0000:00:14.0-3/input0"));
  }

  TEST(InputSeatIsolationSysfs, DerivesThePhysPathFromAnEventNode) {
    EXPECT_EQ(sysfs_phys_path_for_node("/dev/input/event7"), "/sys/class/input/event7/device/phys");
    EXPECT_EQ(sysfs_phys_path_for_node("event12"), "/sys/class/input/event12/device/phys");
  }

  TEST(InputSeatIsolationSysfs, RefusesAnythingThatIsNotAnEventNode) {
    // A surprising node name must be rejected rather than joined onto the sysfs
    // root, so nothing can walk out of the tree.
    EXPECT_TRUE(sysfs_phys_path_for_node("/dev/input/mouse0").empty());
    EXPECT_TRUE(sysfs_phys_path_for_node("/dev/input/event").empty());
    EXPECT_TRUE(sysfs_phys_path_for_node("../../etc/shadow").empty());
    EXPECT_TRUE(sysfs_phys_path_for_node("event7/../../..").empty());
    EXPECT_TRUE(sysfs_phys_path_for_node("").empty());
  }

  TEST(InputSeatIsolationEvaluation, StaysQuietWhenIsolationWasNeverRequested) {
    const auto status = evaluate_isolation(false, {{"event7", ""}});
    EXPECT_FALSE(status.requested);
    EXPECT_FALSE(status.effective);
    EXPECT_TRUE(isolation_warning(status, "mouse").empty());
  }

  TEST(InputSeatIsolationEvaluation, IsEffectiveOnlyWhenEveryDeviceCarriesTheMarker) {
    const auto all_marked = evaluate_isolation(true, {
      {"event7", "polaris/client-input-seat-isolated/mouse"},
      {"event8", "polaris/client-input-seat-isolated/mouse"},
    });
    EXPECT_TRUE(all_marked.effective);
    EXPECT_TRUE(all_marked.unmarked_nodes.empty());
    EXPECT_TRUE(isolation_warning(all_marked, "mouse").empty());

    const auto one_missing = evaluate_isolation(true, {
      {"event7", "polaris/client-input-seat-isolated/mouse"},
      {"event8", ""},
    });
    EXPECT_FALSE(one_missing.effective);
    EXPECT_EQ(one_missing.unmarked_nodes, std::vector<std::string> {"event8"});
  }

  TEST(InputSeatIsolationEvaluation, DoesNotCallAnUnprovenRequestEffective) {
    // No devices means nothing demonstrated the boundary exists.
    const auto status = evaluate_isolation(true, {});
    EXPECT_TRUE(status.requested);
    EXPECT_FALSE(status.effective);
  }

  TEST(InputSeatIsolationEvaluation, WarnsThatTheSavedSettingDoesNothing) {
    const auto status = evaluate_isolation(true, {{"event7", ""}});
    const auto warning = isolation_warning(status, "keyboard");
    ASSERT_FALSE(warning.empty());
    EXPECT_NE(warning.find("client_keyboard_mouse_seat_isolation"), std::string::npos);
    EXPECT_NE(warning.find("keyboard"), std::string::npos);
    EXPECT_NE(warning.find("no effect on this host"), std::string::npos);
  }

  TEST(InputSeatIsolationInspection, TreatsAnUnreadableDeviceAsUnisolated) {
    // sysfs having nothing to say is not proof of isolation.
    const phys_reader_t silent = [](std::string_view) {
      return std::nullopt;
    };
    const auto status = inspect_devices(true, {"event7"}, silent);
    EXPECT_FALSE(status.effective);
    EXPECT_EQ(status.unmarked_nodes, std::vector<std::string> {"event7"});
  }

  TEST(InputSeatIsolationInspection, ReadsEachNodeThroughTheInjectedReader) {
    const phys_reader_t marked = [](std::string_view node) -> std::optional<std::string> {
      return std::string {client_input_phys_marker(true, "mouse")} + std::string {node.empty() ? "" : ""};
    };
    const auto status = inspect_devices(true, {"event7", "event8"}, marked);
    EXPECT_TRUE(status.effective);
  }

}  // namespace

#endif  // __linux__
