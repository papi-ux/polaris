/** Stable Spaces GPU identity: current DRM nodes for a saved PCI address. */
#pragma once
#ifdef __linux__
#include "multiseat_controller_production.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace multiseat::spaces {
  // The kernel numbers /dev/dri/cardN and renderDN in probe order, so an EVDI display or another
  // GPU that probes first moves them on the next boot. A saved GPU is identified by its PCI
  // address, and its DRM nodes are looked up again from sysfs at every start.
  struct drm_node_t {
    std::filesystem::path path;  // always /dev/dri/<kernel name>, never a by-path link
    std::uint32_t major = 0, minor = 0;  // from sysfs "dev"; the /dev node must match
    bool operator==(const drm_node_t &) const = default;
  };
  struct drm_nodes_t {
    std::optional<drm_node_t> card, render;
    bool operator==(const drm_nodes_t &) const = default;
  };
  // "pci-0000_01_00.0", as guided setup saves it, to "0000:01:00.0". Anything else has no address.
  std::optional<std::string> pci_address_of(std::string_view logical_gpu_id);
  // Pure over pci_devices: reads only <pci_devices>/<address>/drm, the DRM minors the kernel bound
  // to that exact device, so a node another device owns is never returned. Empty when the device,
  // its DRM directory or a readable "dev" is missing, or it lists two nodes of one kind.
  std::optional<drm_nodes_t> resolve_drm_nodes(std::string_view pci_address,
    const std::filesystem::path &pci_devices = "/sys/bus/pci/devices");
  struct drm_refresh_t {
    bool ok = false;
    std::filesystem::path unresolved;  // a saved node whose kind the device no longer has
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved;  // saved, current
  };
  // Pure. Rewrites the saved /dev/dri/cardN and renderDN entries, and render_node, to the node of
  // the same kind in current. Other devices, such as NVIDIA's, stay exactly as saved. Refuses when
  // render_node is not a render node or a saved kind is missing now.
  drm_refresh_t refresh_drm_nodes(production_controller_gpu_t &gpu, const drm_nodes_t &current);
}
#endif
