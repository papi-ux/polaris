#include "spaces_gpu_nodes.h"
#ifdef __linux__
#include <algorithm>
#include <charconv>
#include <fstream>
#include <regex>
#include <set>

namespace multiseat::spaces {
  namespace {
    namespace fs = std::filesystem;
    enum class kind_e { other, card, render };
    const std::regex &address_form() {
      static const std::regex form("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]");
      return form;
    }
    kind_e kind_of_name(const std::string &name) {
      static const std::regex card("card[0-9]{1,4}"), render("renderD[0-9]{1,4}");
      if (std::regex_match(name, card)) return kind_e::card;
      if (std::regex_match(name, render)) return kind_e::render;
      return kind_e::other;
    }
    kind_e kind_of_path(const fs::path &path) {
      return path.parent_path() == "/dev/dri" ? kind_of_name(path.filename().string()) : kind_e::other;
    }
    // sysfs "dev" is "<major>:<minor>\n".
    std::optional<std::pair<std::uint32_t, std::uint32_t>> device_number(const fs::path &path) {
      std::ifstream file(path);
      std::string text(33, '\0');
      file.read(text.data(), text.size());
      text.resize(file.gcount());
      if (text.size() > 32) return std::nullopt;
      if (!text.empty() && text.back() == '\n') text.pop_back();
      const auto colon = text.find(':');
      if (colon == std::string::npos) return std::nullopt;
      std::uint32_t major = 0, minor = 0;
      const auto *end = text.data() + text.size();
      const auto first = std::from_chars(text.data(), text.data() + colon, major);
      const auto second = std::from_chars(text.data() + colon + 1, end, minor);
      if (colon == 0 || first.ec != std::errc {} || first.ptr != text.data() + colon ||
          colon + 1 == text.size() || second.ec != std::errc {} || second.ptr != end) return std::nullopt;
      return std::pair {major, minor};
    }
  }

  std::optional<std::string> pci_address_of(std::string_view logical_gpu_id) {
    if (!logical_gpu_id.starts_with("pci-")) return std::nullopt;
    std::string address(logical_gpu_id.substr(4));
    // Guided setup writes the address with '_' for ':'; only those two positions may carry one.
    if (address.size() != 12 || address[4] != '_' || address[7] != '_') return std::nullopt;
    address[4] = address[7] = ':';
    if (!std::regex_match(address, address_form())) return std::nullopt;
    return address;
  }

  std::optional<drm_nodes_t> resolve_drm_nodes(std::string_view pci_address, const fs::path &pci_devices) {
    const std::string address(pci_address);
    if (!std::regex_match(address, address_form())) return std::nullopt;
    std::error_code error;
    drm_nodes_t nodes;
    for (fs::directory_iterator entry(pci_devices / address / "drm", error), end; !error && entry != end;
         entry.increment(error)) {
      const auto name = entry->path().filename().string();
      const auto kind = kind_of_name(name);
      if (kind == kind_e::other) continue;  // controlD links, connectors and attributes
      // A DRM minor is a directory the kernel creates inside its own device. A link under that
      // name would point somewhere else, so the device's layout cannot be trusted.
      if (fs::symlink_status(entry->path(), error).type() != fs::file_type::directory) return std::nullopt;
      const auto number = device_number(entry->path() / "dev");
      auto &slot = kind == kind_e::card ? nodes.card : nodes.render;
      if (!number || slot) return std::nullopt;
      slot = drm_node_t {fs::path("/dev/dri") / name, number->first, number->second};
    }
    if (error || (!nodes.card && !nodes.render)) return std::nullopt;
    return nodes;
  }

  drm_refresh_t refresh_drm_nodes(production_controller_gpu_t &gpu, const drm_nodes_t &current) {
    drm_refresh_t result;
    if (kind_of_path(gpu.render_node) != kind_e::render) {
      result.unresolved = gpu.render_node;
      return result;
    }
    auto updated = gpu;
    const auto replace = [&](fs::path &path) {
      const auto kind = kind_of_path(path);
      if (kind == kind_e::other) return true;
      const auto &node = kind == kind_e::card ? current.card : current.render;
      if (!node) {
        result.unresolved = path;
        return false;
      }
      if (path != node->path) {
        const std::pair change {path, node->path};
        if (std::find(result.moved.begin(), result.moved.end(), change) == result.moved.end()) result.moved.push_back(change);
        path = node->path;
      }
      return true;
    };
    if (!replace(updated.render_node)) return result;
    for (auto &device : updated.devices) {
      if (!replace(device)) return result;
    }
    // Two saved nodes of one kind cannot both be this device's; they would collapse onto one path.
    if (std::set<fs::path>(updated.devices.begin(), updated.devices.end()).size() != updated.devices.size()) {
      result.moved.clear();
      return result;
    }
    gpu = std::move(updated);
    result.ok = true;
    return result;
  }
}
#endif
