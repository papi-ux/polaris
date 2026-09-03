/**
 * @file src/platform/linux/input/input_seat_isolation.h
 * @brief Seat isolation identities for the virtual keyboard, mouse, touch and pen.
 *
 * A Polaris host can run a private stream session while someone is using the
 * desktop session logged in at the machine. Without isolation both sessions see
 * the same devices: the client's typing and pointer movement drive the host
 * desktop as well as the stream.
 *
 * Tagging the virtual devices with a phys marker lets the bundled udev rules
 * assign them to a dedicated seat, which stops logind from handing them to the
 * session at seat0. This is the same mechanism the client gamepads use, and it
 * carries the same boundary: it separates seats, not processes. Applications
 * running under the same Unix account, and members of the `input` group, still
 * reach the devices directly.
 */
#pragma once

#include <cctype>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platf::input_isolation {

  /// Marker written to the device's phys attribute; matched by 60-polaris.rules.
  constexpr std::string_view seat_isolated_phys_prefix = "polaris/client-input-seat-isolated/";

  /**
   * @brief Build the phys marker for a virtual input device.
   * @param enabled Whether client keyboard/mouse seat isolation is enabled.
   * @param device Short device kind, e.g. "mouse" or "keyboard".
   * @return The marker, or an empty string when isolation is disabled.
   */
  inline std::string client_input_phys_marker(bool enabled, std::string_view device) {
    if (!enabled) {
      return {};
    }

    return std::string {seat_isolated_phys_prefix} + std::string {device};
  }

  /**
   * @brief Whether a kernel-reported phys value carries the isolation marker.
   *
   * The udev rules match on this prefix, so a device whose phys does not start
   * with it will never be moved off seat0 no matter what was requested.
   */
  inline bool phys_carries_isolation_marker(std::string_view phys) {
    return phys.rfind(seat_isolated_phys_prefix, 0) == 0;
  }

  /**
   * @brief Sysfs path holding the kernel-visible phys attribute for an event node.
   * @param node_path Device node, either `/dev/input/eventN` or a bare `eventN`.
   * @return The sysfs path, or an empty string when the node is not an event node.
   *
   * Anything that is not a plain `eventN` name is rejected rather than joined,
   * so a surprising node name cannot walk out of the sysfs tree.
   */
  inline std::string sysfs_phys_path_for_node(std::string_view node_path, std::string_view sysfs_root = "/sys/class/input") {
    const auto slash = node_path.find_last_of('/');
    const auto name = slash == std::string_view::npos ? node_path : node_path.substr(slash + 1);
    constexpr std::string_view event_prefix = "event";
    if (name.rfind(event_prefix, 0) != 0 || name.size() <= event_prefix.size()) {
      return {};
    }

    for (const char digit : name.substr(event_prefix.size())) {
      if (!std::isdigit(static_cast<unsigned char>(digit))) {
        return {};
      }
    }

    return std::string {sysfs_root} + "/" + std::string {name} + "/device/phys";
  }

  /// What the kernel actually did with a seat isolation request.
  struct isolation_status_t {
    bool requested = false;  ///< The operator asked for isolation.
    bool effective = false;  ///< Every inspected device carries the marker.
    std::vector<std::string> unmarked_nodes;  ///< Nodes the kernel reports without it.
  };

  /**
   * @brief Decide whether a seat isolation request took effect.
   * @param requested Whether the setting is enabled.
   * @param phys_by_node Each device node paired with the phys the kernel reports.
   *
   * Pure so the decision can be tested without a filesystem. A request with no
   * devices to inspect is not called effective: nothing has proven it works.
   */
  inline isolation_status_t evaluate_isolation(bool requested, const std::vector<std::pair<std::string, std::string>> &phys_by_node) {
    isolation_status_t status;
    status.requested = requested;
    if (!requested) {
      return status;
    }

    for (const auto &[node, phys] : phys_by_node) {
      if (!phys_carries_isolation_marker(phys)) {
        status.unmarked_nodes.push_back(node);
      }
    }

    status.effective = !phys_by_node.empty() && status.unmarked_nodes.empty();
    return status;
  }

  /// Reads a device's phys attribute. Returns nullopt when sysfs has nothing to say.
  using phys_reader_t = std::function<std::optional<std::string>(std::string_view)>;

  /// Default reader: the first line of the device's sysfs phys attribute.
  inline std::optional<std::string> read_device_phys(std::string_view node_path) {
    const auto path = sysfs_phys_path_for_node(node_path);
    if (path.empty()) {
      return std::nullopt;
    }

    std::ifstream file {path};
    if (!file) {
      return std::nullopt;
    }

    std::string phys;
    std::getline(file, phys);
    while (!phys.empty() && (phys.back() == '\n' || phys.back() == '\r' || phys.back() == ' ')) {
      phys.pop_back();
    }
    return phys;
  }

  /**
   * @brief Inspect live device nodes and report whether isolation took effect.
   *
   * A node sysfs cannot answer for is treated as unmarked, because an unproven
   * boundary is not a boundary.
   */
  inline isolation_status_t inspect_devices(bool requested, const std::vector<std::string> &nodes, const phys_reader_t &reader = read_device_phys) {
    std::vector<std::pair<std::string, std::string>> phys_by_node;
    phys_by_node.reserve(nodes.size());
    for (const auto &node : nodes) {
      phys_by_node.emplace_back(node, reader(node).value_or(std::string {}));
    }

    return evaluate_isolation(requested, phys_by_node);
  }

  /**
   * @brief One-line explanation for an isolation request that did not take.
   * @return Empty when isolation is off, or when it is on and working.
   *
   * Polaris fills in device_phys on every virtual keyboard, mouse, touch and pen
   * it creates, but a uinput backend that drops the field leaves the devices on
   * seat0 with nothing in the logs to say so. This is that missing sentence.
   */
  inline std::string isolation_warning(const isolation_status_t &status, std::string_view device_label) {
    if (!status.requested || status.effective) {
      return {};
    }

    return "client_keyboard_mouse_seat_isolation is enabled, but the virtual " + std::string {device_label} +
           " reports no isolation marker to the kernel, so logind still hands it to the desktop session at seat0. "
           "The setting is saved and has no effect on this host.";
  }

}  // namespace platf::input_isolation
